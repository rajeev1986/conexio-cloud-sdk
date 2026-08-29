/*
 * cell_location.c — Cellular positioning for AWS Location Service
 *
 * Copyright (c) 2026 Conexio Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Collects LTE neighbour cell data via AT%NCELLMEAS and publishes it as
 * telemetry metrics. The cloud Lambda resolves the cell data to coordinates
 * using a positioning solver (HERE / Combain / Google Geolocation) and then
 * updates an AWS Location Tracker resource.
 *
 * Metric names sent in telemetry:
 *
 *   _loc_mcc          Mobile Country Code of serving cell  (int)
 *   _loc_mnc          Mobile Network Code of serving cell  (int)
 *   _loc_cell_id      E-UTRAN Cell ID of serving cell      (int)
 *   _loc_tac          Tracking Area Code                   (int)
 *   _loc_earfcn       EARFCN of serving cell               (int)
 *   _loc_rsrp         RSRP of serving cell (dBm)           (int)
 *   _loc_timing_adv   Timing advance (distance proxy)      (int, optional)
 *   _loc_neighbors    JSON array of neighbour cells        (string)
 *                     [{"earfcn":5110,"pci":42,"rsrp":-85},...]
 *
 * All metrics use the "_loc_" prefix so the cloud can identify them as
 * location-related and route them to the positioning Lambda.
 *
 * Measurement rate:
 *   Controlled by CONFIG_CELL_LOCATION_INTERVAL_SEC (default: 28800 s = 8 h).
 *   cell_location_tick() is called by the SDK background loop every second
 *   and fires cell_location_request() when the countdown reaches zero.
 *   This decouples the location fix rate from the telemetry publish rate
 *   so you can publish frequently while only calling the positioning API
 *   a few times per day, keeping HERE / AWS Location costs minimal.
 *
 * Power note:
 *   AT%NCELLMEAS keeps the radio active for 1–5 seconds per measurement.
 *   At the default 8-hour interval the power impact is negligible.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <modem/lte_lc.h>
#include <cJSON.h>
#include <cJSON_os.h>
#include <string.h>
#include <conexio_cloud/conexio_cloud.h>
#include <conexio_cloud/cell_location.h>
#include "transport.h"   /* conexio_cloud_publish_single(), TOPIC_CAT_LOCATION */

LOG_MODULE_REGISTER(cell_location, LOG_LEVEL_INF);

/* ── State ────────────────────────────────────────────────────────────── */
static bool     g_initialised        = false;
static bool     g_measurement_active = false;
static uint32_t g_tick_counter       = 0;   /* seconds since last measurement */

/* ── Deferred publish work item ───────────────────────────────────────
 *
 * The LTE LC event handler runs in the LTE LC system work queue context.
 * Calling conexio_cloud_publish() directly from there is unsafe — it can
 * block and re-enter MQTT paths that expect to run from a different thread.
 *
 * Instead, schedule a k_work item that runs in the system workqueue and
 * calls conexio_cloud_publish() safely after all _loc_* metrics are queued.
 */
#if defined(CONFIG_CELL_LOCATION_PUBLISH_ON_FIX)
static void publish_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_INF("cell_location: publishing location fix (location topic only)");
	/* Publish ONLY the location category — not all 4 topics.
	 * Calling conexio_cloud_publish() here would re-send diagnostics and
	 * telemetry, causing duplicate boot-once metrics on the first fix. */
	int ret = conexio_cloud_publish_single(TOPIC_CAT_LOCATION);
	if (ret) {
		LOG_WRN("cell_location: location publish failed (%d) — "
			"metrics will ride next scheduled interval", ret);
	}
}
static K_WORK_DEFINE(g_publish_work, publish_work_handler);
#endif

/* ── RSRP raw index → dBm ─────────────────────────────────────────────
 * Per 3GPP TS 36.133: RSRP_dBm = index - 141  (NCS convention)
 * RSRP_IDX_TO_DBM is defined in modem/lte_lc.h in recent NCS versions.
 */
#ifndef RSRP_IDX_TO_DBM
#  define RSRP_IDX_TO_DBM(x) ((x) < 0 ? (x) - 140 : (x) - 141)
#endif

/* ── Serialize neighbour cells to compact JSON string ──────────────────
 *
 * Format: [{"earfcn":5110,"pci":42,"rsrp":-85},{"earfcn":5110,"pci":17,"rsrp":-90}]
 *
 * Kept compact (no whitespace) to minimise telemetry payload size.
 * Lambda parses this as a JSON array for the positioning API call.
 *
 * @param neighbors   Pointer to neighbour cell array
 * @param count       Number of entries
 * @param buf         Output buffer
 * @param buf_size    Size of output buffer
 * @return true on success, false if buffer too small or serialization failed
 */
static bool serialize_neighbors(const struct lte_lc_ncell *neighbors,
				uint8_t count,
				char *buf, size_t buf_size)
{
	if (!neighbors || count == 0 || !buf || buf_size == 0) {
		if (buf && buf_size > 2) {
			buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
		}
		return true;
	}

	cJSON *arr = cJSON_CreateArray();
	if (!arr) {
		return false;
	}

	for (uint8_t i = 0; i < count; i++) {
		const struct lte_lc_ncell *n = &neighbors[i];
		cJSON *cell = cJSON_CreateObject();
		if (!cell) {
			cJSON_Delete(arr);
			return false;
		}

		cJSON_AddNumberToObject(cell, "earfcn", (double)n->earfcn);
		cJSON_AddNumberToObject(cell, "pci",    (double)n->phys_cell_id);
		if (n->rsrp != LTE_LC_CELL_RSRP_INVALID) {
			cJSON_AddNumberToObject(cell, "rsrp",
						(double)RSRP_IDX_TO_DBM(n->rsrp));
		}
		cJSON_AddItemToArray(arr, cell);
	}

	char *json_str = cJSON_PrintUnformatted(arr);
	cJSON_Delete(arr);

	if (!json_str) {
		return false;
	}

	size_t len = strlen(json_str);
	bool ok = (len < buf_size);

	if (ok) {
		memcpy(buf, json_str, len + 1);
	} else {
		LOG_WRN("cell_location: neighbour buffer too small (%zu < %zu) — "
			"truncating to empty array", buf_size, len + 1);
		buf[0] = '['; buf[1] = ']'; buf[2] = '\0';
	}

	cJSON_free(json_str);
	return ok;
}

/* ── Process measurement result and queue telemetry metrics ────────────
 *
 * Called from the LTE LC event handler when NCELLMEAS completes.
 * Queues all location-related metrics via conexio_cloud_send_metric_*()
 * so they ride alongside the next regular telemetry publish.
 */
static void process_cell_measurement(const struct lte_lc_cells_info *cells)
{
	const struct lte_lc_cell *sc = &cells->current_cell;

	/* Validate serving cell — id == INVALID means the measurement failed */
	if (sc->id == LTE_LC_CELL_EUTRAN_ID_INVALID) {
		LOG_WRN("cell_location: measurement returned invalid cell ID — skipping");
		return;
	}

	LOG_INF("cell_location: measurement complete — MCC=%d MNC=%d CellID=%u "
		"TAC=%u EARFCN=%u RSRP=%d dBm neighbors=%u",
		sc->mcc, sc->mnc, sc->id, sc->tac, sc->earfcn,
		RSRP_IDX_TO_DBM(sc->rsrp), cells->ncells_count);

	/* ── Serving cell metrics ─────────────────────────────────────── */
	conexio_cloud_send_metric("_loc_mcc",     (double)sc->mcc);
	conexio_cloud_send_metric("_loc_mnc",     (double)sc->mnc);
	conexio_cloud_send_metric("_loc_cell_id", (double)sc->id);
	conexio_cloud_send_metric("_loc_tac",     (double)sc->tac);
	conexio_cloud_send_metric("_loc_earfcn",  (double)sc->earfcn);

	if (sc->rsrp != LTE_LC_CELL_RSRP_INVALID) {
		conexio_cloud_send_metric("_loc_rsrp",
					  (double)RSRP_IDX_TO_DBM(sc->rsrp));
	}

	/* Timing advance — useful for distance estimation (~100 m resolution).
	 * Only send when valid (INVALID means the modem doesn't have a fresh
	 * value yet). */
	if (sc->timing_advance != LTE_LC_CELL_TIMING_ADVANCE_INVALID) {
		conexio_cloud_send_metric("_loc_timing_adv",
					  (double)sc->timing_advance);
	}

	/* ── Neighbour cells (JSON string metric) ─────────────────────── */
	if (cells->ncells_count > 0 && cells->neighbor_cells) {
		/* Each neighbour ~30 chars + overhead. 17 neighbours max ≈ 640 B */
		char nbuf[640];

		if (serialize_neighbors(cells->neighbor_cells,
					cells->ncells_count,
					nbuf, sizeof(nbuf))) {
			conexio_cloud_send_metric_str("_loc_nbrs", nbuf);
			LOG_DBG("cell_location: queued %u neighbour cells",
				cells->ncells_count);
		}
	}

	LOG_INF("cell_location: location metrics queued — publishing immediately");

#if defined(CONFIG_CELL_LOCATION_PUBLISH_ON_FIX)
	/* Schedule publish in system workqueue — safe to call from LTE LC
	 * event handler context. submit is a no-op if already pending. */
	k_work_submit(&g_publish_work);
#else
	LOG_INF("cell_location: publish-on-fix disabled — metrics ride next interval");
#endif
}

/* ── LTE LC event handler ──────────────────────────────────────────────
 *
 * Registered via lte_lc_register_handler(). Receives all LTE LC events
 * but only acts on LTE_LC_EVT_NEIGHBOR_CELL_MEAS.
 */
static void lte_lc_evt_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type != LTE_LC_EVT_NEIGHBOR_CELL_MEAS) {
		return;
	}

	g_measurement_active = false;

	const struct lte_lc_cells_info *cells = &evt->cells_info;

	if (cells->current_cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID) {
		LOG_WRN("cell_location: neighbour cell measurement failed or timed out");
		return;
	}

	process_cell_measurement(cells);
}

/* ── Public API ────────────────────────────────────────────────────────── */

int cell_location_init(void)
{
	if (g_initialised) {
		LOG_DBG("cell_location: already initialised");
		return 0;
	}

	/* lte_lc_register_handler() returns void in NCS v3.2.1. */
	lte_lc_register_handler(lte_lc_evt_handler);

	g_initialised = true;

	LOG_INF("cell_location: initialised — interval %d s, search type %d "
		"(0=default 1=ext_light 2=extended)",
		CONFIG_CELL_LOCATION_INTERVAL_SEC,
		CONFIG_CELL_LOCATION_SEARCH_TYPE);
	return 0;
}

int cell_location_request(void)
{
	if (!g_initialised) {
		LOG_ERR("cell_location: not initialised — call cell_location_init() first");
		return -EINVAL;
	}

	if (g_measurement_active) {
		LOG_DBG("cell_location: measurement already in progress — skipping");
		return -EBUSY;
	}

	struct lte_lc_ncellmeas_params params = {
		.search_type = (enum lte_lc_neighbor_search_type)CONFIG_CELL_LOCATION_SEARCH_TYPE,
		.gci_count   = 0,
	};

	int ret = lte_lc_neighbor_cell_measurement(&params);
	if (ret) {
		LOG_ERR("cell_location: lte_lc_neighbor_cell_measurement failed (%d)", ret);
		return ret;
	}

	g_measurement_active = true;
	g_tick_counter = 0;   /* reset countdown so next fires after full interval */

	LOG_INF("cell_location: AT%%NCELLMEAS started — awaiting result...");
	return 0;
}

void cell_location_tick(void)
{
	if (!g_initialised || g_measurement_active) {
		return;
	}

	g_tick_counter++;

	if (g_tick_counter >= (uint32_t)CONFIG_CELL_LOCATION_INTERVAL_SEC) {
		g_tick_counter = 0;
		LOG_DBG("cell_location: interval elapsed — requesting measurement");
		cell_location_request();
	}
}

bool cell_location_is_busy(void)
{
	return g_measurement_active;
}
