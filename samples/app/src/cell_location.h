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
 * │    → N neighbors        _loc_cell_id, _loc_tac    Position             │
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
 * Usage:
 *   // Call once after LTE is connected and before main loop
 *   cell_location_init();
 *
 *   // Call whenever you want a location fix (e.g. on a timer, on demand)
 *   cell_location_request();
 *   // Metrics are queued automatically — next telemetry publish sends them
 */

#ifndef CELL_LOCATION_H_
#define CELL_LOCATION_H_

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
 * @return 0 on success, negative errno on failure.
 */
int cell_location_init(void);

/**
 * @brief Request a neighbor cell measurement.
 *
 * Triggers AT%NCELLMEAS. The result arrives asynchronously via
 * LTE_LC_EVT_NEIGHBOR_CELL_MEAS and is automatically queued as telemetry
 * metrics via conexio_cloud_send_metric().
 *
 * Non-blocking — returns immediately. The metrics will appear in the next
 * telemetry publish after the measurement completes (~1–5 seconds).
 *
 * Safe to call from any thread context.
 *
 * @return 0 on success, negative errno if measurement could not be started.
 */
int cell_location_request(void);

/**
 * @brief Returns true if a neighbor cell measurement is currently in progress.
 */
bool cell_location_is_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* CELL_LOCATION_H_ */
