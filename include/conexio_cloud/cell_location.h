/*
 * cell_location.h — Cellular positioning for AWS Location Service
 *
 * Copyright (c) 2026 Conexio Technologies, Inc.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ┌────────────────────────────────────────────────────────────────────────┐
 * │  Architecture                                                          │
 * │                                                                        │
 * │  Device                 Cloud Lambda              AWS Location         │
 * │  ──────                 ─────────────             ────────────         │
 * │  AT%NCELLMEAS           receive telemetry         Tracker resource     │
 * │    → serving cell   ──► _loc_mcc, _loc_mnc    ──► BatchUpdateDevice   │
 * │    → N neighbours       _loc_cell_id, _loc_tac    Position             │
 * │    → RSRP / earfcn      _loc_rsrp                                     │
 * │                         _loc_neighbors (JSON)                          │
 * │                         call HERE / Combain API                        │
 * │                         get {lat, lng, accuracy}                       │
 * │                         push to AWS Location                           │
 * └────────────────────────────────────────────────────────────────────────┘
 *
 * The device side is intentionally thin:
 *   1. Trigger AT%NCELLMEAS when a location fix is wanted
 *   2. Collect the measurement result from the LTE LC event
 *   3. Serialize the data as telemetry metrics and queue them for publish
 *
 * No positioning solver runs on the device — the Lambda handles that.
 * This keeps the firmware simple and solver-agnostic.
 *
 * The measurement interval is controlled by CONFIG_CELL_LOCATION_INTERVAL_SEC
 * (default 28800 s = 8 hours). This is independent from the telemetry publish
 * interval so you can publish telemetry frequently while only resolving
 * location a few times per day, keeping API costs minimal.
 *
 * When CONFIG_CELL_LOCATION_PUBLISH_ON_FIX=y (default), the SDK triggers an
 * immediate conexio_cloud_publish() as soon as the _loc_* metrics are queued,
 * via a k_work item in the system workqueue. This ensures the dashboard sees
 * the new location data right away rather than waiting hours for the next
 * scheduled publish interval.
 *
 * NOTE: Do NOT call cell_location_request() immediately after boot. The boot
 * publish fires first and the location k_work would trigger a second publish
 * within milliseconds, duplicating diagnostics and telemetry. Let
 * cell_location_tick() fire the first measurement after the configured
 * interval instead.
 *
 * Usage:
 *   #include <conexio_cloud/cell_location.h>
 *
 *   // Call once after LTE is connected (after conexio_cloud_init returns)
 *   cell_location_init();
 *
 *   // The SDK calls cell_location_tick() from its background loop.
 *   // You do not need to call cell_location_request() manually unless
 *   // you want an on-demand fix (e.g. on a button press or geofence event).
 */

#ifndef CONEXIO_CLOUD_CELL_LOCATION_H_
#define CONEXIO_CLOUD_CELL_LOCATION_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise the cell location module.
 *
 * Registers the LTE LC event handler that listens for
 * LTE_LC_EVT_NEIGHBOR_CELL_MEAS results. Must be called after LTE is
 * connected (after conexio_cloud_init() returns).
 *
 * Safe to call multiple times — subsequent calls are no-ops.
 *
 * @return 0 on success, negative errno on failure.
 */
int cell_location_init(void);

/**
 * @brief Request a neighbour cell measurement immediately.
 *
 * Triggers AT%NCELLMEAS. The result arrives asynchronously via
 * LTE_LC_EVT_NEIGHBOR_CELL_MEAS and is automatically queued as telemetry
 * metrics via conexio_cloud_send_metric().
 *
 * Non-blocking — returns immediately. The metrics appear in the next
 * telemetry publish after the measurement completes (~1–5 seconds).
 *
 * The SDK calls this automatically at the rate set by
 * CONFIG_CELL_LOCATION_INTERVAL_SEC. Call it directly only when you need
 * an on-demand fix outside the regular schedule.
 *
 * @return 0 on success, -EBUSY if a measurement is already in progress,
 *         negative errno on other errors.
 */
int cell_location_request(void);

/**
 * @brief Interval tick — called by the SDK background loop every second.
 *
 * Drives the CONFIG_CELL_LOCATION_INTERVAL_SEC countdown and fires
 * cell_location_request() when the interval expires. You do not need to
 * call this directly; the SDK calls it automatically.
 */
void cell_location_tick(void);

/**
 * @brief Returns true if a neighbour cell measurement is currently in progress.
 */
bool cell_location_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* CONEXIO_CLOUD_CELL_LOCATION_H_ */
