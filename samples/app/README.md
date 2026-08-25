# Conexio Advanced Sample App

Production-grade firmware for **Conexio Stratus Pro** (nRF9151) built on
`conexio-cloud-sdk` with nRF Connect SDK v3.2.1.

The SDK handles all infrastructure — PSM, offline buffering, FOTA, retry/watchdog,
NTP sync, certificate management, MQTT command delivery, and battery metrics.
The application provides sensor callbacks, command handlers, settings handlers,
and the cellular location module.

---

## Table of Contents

1. [Quick Start](#quick-start)
2. [Application Structure](#application-structure)
3. [Telemetry Data Packet](#telemetry-data-packet)
4. [Cellular Location Feature](#cellular-location-feature)
5. [HERE Positioning API — Pricing](#here-positioning-api--pricing)
6. [Firmware Configuration Reference](#firmware-configuration-reference)
7. [Build Variants](#build-variants)
8. [Commands and Settings](#commands-and-settings)
9. [Battery Metrics (nPM1300)](#battery-metrics-npm1300)
10. [FOTA — Firmware Over-the-Air](#fota--firmware-over-the-air)
11. [Power Management](#power-management)
12. [Offline Buffering](#offline-buffering)

---

## Quick Start

```bash
# Build for Conexio Stratus Pro
west build -b conexio_stratus_pro/nrf9151/ns -- \
  -DEXTRA_CONF_FILE=low_power.conf   # optional: disable UART for field builds

# Flash
west flash

# Monitor serial output (development)
west espressif monitor   # or: screen /dev/tty.usbmodem* 115200
```

---

## Application Structure

```
src/
  main.c            — SDK init, sensor/command/settings registration, main loop
  cell_location.c   — Cellular positioning: AT%NCELLMEAS → telemetry metrics
  cell_location.h   — Public API: init / request / is_busy
  fuel_gauge.c/h    — nPM1300 fuel gauge driver wrapper
prj.conf            — All Kconfig options with inline documentation
low_power.conf      — Production overlay: disables UART/logging
debug.conf          — Development overlay: DBG-level logging + assertions
VERSION             — App firmware version (published as _app_fw_version)
```

The application uses a single SDK include:

```c
#include <conexio_cloud/conexio_cloud.h>
```

Everything else — MQTT, TLS, modem, NTP, PSM, FOTA, retry — is auto-selected
by the SDK Kconfig.

---

## Telemetry Data Packet

Each telemetry publish sends a JSON object to the cloud over MQTT.

### Full Packet Example

```json
{
  "type": "telemetry",
  "deviceId": "355025934980275",
  "timestamp": "2026-08-24T17:29:20.929Z",
  "metrics": {
    "_rssi": 57,
    "_snr": 9,
    "_lte_band": 13,
    "_operator": "VZW",
    "_modem_fw": "nrf91x1_2.0.4",
    "_sdk_version": "2.1.0",
    "_app_fw_version": "1.0.4",
    "_modem_temp": 25,
    "_tx_kb": 3,
    "_rx_kb": 5,
    "_conn_loss": 0,
    "_reboot_cnt": 28,
    "_reboot_reason": "pin",
    "_lte_connect_ms": 6067,
    "_lte_mode": 7,
    "_psm_tau_sec": 11160,
    "_psm_active_sec": 60,
    "_cell_id": 129061889,
    "_tac": 52228,
    "_battery_mv": 4046.99,
    "_battery_soc_pct": 85.48,
    "_battery_drain_pct_hr": 0.42,
    "temperature": 29.5,
    "humidity": 71.0,
    "_loc_mcc": 311,
    "_loc_mnc": 480,
    "_loc_cell_id": 129061889,
    "_loc_tac": 52228,
    "_loc_earfcn": 5230,
    "_loc_rsrp": -85,
    "_loc_timing_adv": 16
  }
}
```

### Metric Reference

| Metric | Source | Description |
|--------|--------|-------------|
| `_rssi` | SDK auto | Signal strength (dBm) |
| `_snr` | SDK auto | Signal-to-noise ratio (dB) |
| `_lte_band` | SDK auto | Active LTE band number |
| `_operator` | SDK auto | Network operator name |
| `_modem_fw` | SDK auto | Modem firmware version (boot only) |
| `_sdk_version` | SDK auto | Conexio Cloud SDK version |
| `_app_fw_version` | SDK auto | App firmware version from `VERSION` file |
| `_modem_temp` | SDK auto | Modem die temperature (°C) |
| `_tx_kb` / `_rx_kb` | SDK auto | Data transferred since last publish (KB) |
| `_conn_loss` | SDK auto | Connection loss count since last publish |
| `_reboot_cnt` | SDK auto | Total reboot counter (NVS-persisted) |
| `_reboot_reason` | SDK auto | Last reboot cause: `por`, `pin`, `wdt`, `soft`, `srst` |
| `_lte_connect_ms` | SDK auto | Time to LTE attach (ms) |
| `_lte_mode` | SDK auto | LTE mode bitmask |
| `_psm_tau_sec` | SDK auto | Negotiated PSM TAU timer (s) |
| `_psm_active_sec` | SDK auto | Negotiated PSM active time (s) |
| `_cell_id` | SDK auto | Serving E-UTRAN Cell ID |
| `_tac` | SDK auto | Tracking Area Code |
| `_battery_mv` | `main.c` | Battery voltage from nPM1300 (mV) |
| `_battery_soc_pct` | SDK auto | State of charge (0–100%) |
| `_battery_drain_pct_hr` | SDK auto | Average drain rate (%/hour, discharge only) |
| `temperature` | `main.c` | Ambient temperature (°C) |
| `humidity` | `main.c` | Relative humidity (%) |
| `_loc_mcc` | `cell_location.c` | Mobile Country Code |
| `_loc_mnc` | `cell_location.c` | Mobile Network Code |
| `_loc_cell_id` | `cell_location.c` | E-UTRAN Cell ID (serving cell) |
| `_loc_tac` | `cell_location.c` | Tracking Area Code |
| `_loc_earfcn` | `cell_location.c` | EARFCN (frequency channel number) |
| `_loc_rsrp` | `cell_location.c` | RSRP signal strength (dBm) |
| `_loc_timing_adv` | `cell_location.c` | Timing advance (distance proxy, optional) |
| `_loc_neighbors` | `cell_location.c` | JSON array of neighbor cells (optional) |

> **Boot-only metrics** (`_modem_fw`, `_reboot_reason`, `_reboot_cnt`,
> `_lte_connect_ms`, `_psm_tau_sec`, `_psm_active_sec`) are included only
> in the first publish after reset. All other metrics publish every interval.

---

## Cellular Location Feature

### How It Works — End to End

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  nRF9151 Firmware               AWS Lambda              AWS + HERE          │
│  ─────────────────               ──────────              ──────────          │
│                                                                             │
│  1. AT%NCELLMEAS           ──►  2. EventBridge         3. HERE lteCatM API │
│     serving cell MCC/MNC        TelemetryIngested  ──►    mcc, mnc, cid,   │
│     Cell ID, TAC, EARFCN        route to Lambda          rsrp, fallback=area│
│     RSRP, timing advance                                                    │
│     up to 17 neighbor cells  ◄──────────────────────── {lat, lng, accuracy} │
│                                                                             │
│  3. Metrics queued as           4. Lambda writes to    5. Dashboard queries │
│     _loc_* in telemetry    ──►  AWS Location Tracker   GET /location        │
│     ride next publish           BatchUpdateDevice       shows pin on map    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Firmware Side (`cell_location.c`)

The firmware side is intentionally thin — no positioning solver runs on device.

1. `cell_location_init()` — registers the LTE LC event handler for
   `LTE_LC_EVT_NEIGHBOR_CELL_MEAS`. Called once after `conexio_cloud_init()`.

2. `cell_location_request()` — issues `AT%NCELLMEAS` (non-blocking).
   Takes 1–5 seconds. Finds the serving cell and up to 17 neighbor cells.

3. The LTE LC event fires with `lte_lc_cells_info`. The handler serializes
   serving cell data + neighbor cells into `_loc_*` metrics and queues them
   via `conexio_cloud_send_metric()`.

4. The queued metrics ride the next regular telemetry publish — no extra
   MQTT message is needed.

### Location Metrics Published

```c
_loc_mcc         // Mobile Country Code          e.g. 311
_loc_mnc         // Mobile Network Code          e.g. 480 (zero-padded string in Lambda)
_loc_cell_id     // E-UTRAN Cell ID              e.g. 129061889
_loc_tac         // Tracking Area Code           e.g. 52228
_loc_earfcn      // Frequency channel            e.g. 5230
_loc_rsrp        // Signal strength (dBm)        e.g. -85
_loc_timing_adv  // Distance proxy (optional)    e.g. 16
_loc_neighbors   // JSON array (optional)        e.g. [{"earfcn":5110,"pci":42,"rsrp":-90}]
```

### Cloud Side (Lambda: `iot-dashboard-location`)

- Trigger: EventBridge rule on `iot-dashboard-internal` bus,
  `source = iot-dashboard.ingestion`, `detail-type = TelemetryIngested`
- Calls HERE Positioning API using `lteCatM` element (not `lte` — the
  nRF9151 is LTE-M; using `lte` returns HTTP 400)
- Fields sent to HERE: `mcc`, `mnc` (zero-padded string), `cid`, `rsrp`
- `fallback=area` query param added — returns an eNodeB-level estimate
  when the exact cell is not in HERE's database
- On success: calls `BatchUpdateDevicePosition` on AWS Location Tracker
  `iot-dashboard-tracker`
- Result is queryable via `GET /v1/devices/{deviceId}/location`

### How Often to Request a Fix

| Use case | Recommended interval | Rationale |
|----------|---------------------|-----------|
| Development / testing | Every telemetry interval (30–60s) | Verify the pipeline end to end |
| Field tracking | Every 4–8 hours | 2–3 fixes/day per device keeps HERE costs minimal |
| Asset management | Every 1–24 hours | Most assets don't move frequently |

**Current firmware behavior:** `cell_location_request()` is called once per
telemetry interval (every `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` seconds).

**Production plan:** Add `CONFIG_CELL_LOCATION_INTERVAL_SEC` (e.g. `28800`
for 8 hours) to decouple the location fix rate from the telemetry rate.
This keeps the location solver called only 2–3 times per day per device.

### Power Impact

`AT%NCELLMEAS` keeps the radio active for ~1–5 seconds per measurement.
At a 60-second telemetry interval this is negligible. For ultra-low-power
deployments calling it less frequently (every N publishes) reduces the
average current draw.

---

## HERE Positioning API — Pricing

> **Source:** [HERE Base Plan pricing](https://www.here.com/get-started/pricing)
> and [HERE developer billing FAQ](https://docs.here.com/here-kb/docs/how-is-billing-for-a-developer-here-platform-account).
> Verify current numbers on the HERE platform portal — pricing can change.

### Plan Overview

HERE moved to the **Base Plan** in 2025 (the old Limited Plan of 1,000
requests/day was retired on August 31, 2025).

| Plan | Free tier | Overage | Notes |
|------|-----------|---------|-------|
| Base Plan | Free monthly transaction allowance per service | Pay-as-you-grow, per 1,000 transactions | Requires credit card; not charged until free tier exceeded |

The Base Plan is consumption-based — no prepaid credits required. Usage is
metered monthly and invoiced around the 15th of the following month.

### Positioning API Specifically

The HERE Positioning API (the `lteCatM` / cell-based location solver used
by this project) falls under HERE Location Services transactions. The exact
free tier for the Positioning API is shown in your HERE platform account
under **Usage**. Check [platform.here.com](https://platform.here.com) for
the current allowance for your account.

As a rough operational guide based on publicly available information:

- The Base Plan includes **free monthly transaction allowances** for
  location services — sufficient for development and low-volume production
  with a small fleet
- Beyond the free tier, pricing is in the range of **$1 per 1,000
  transactions** (varies by service and volume tier)
- A fleet of 10 devices at 3 fixes/day = **~900 requests/month** — well
  within any reasonable free tier
- A fleet of 100 devices at 3 fixes/day = **~9,000 requests/month** — still
  inexpensive at pay-as-you-go rates (~$9/month at $1/1,000)
- A fleet of 1,000 devices at 3 fixes/day = **~90,000 requests/month** —
  worth negotiating a volume agreement with HERE at this scale

### Cost Reduction Strategies

1. **Rate-limit location fixes** — 2–3 per device per day is sufficient for
   most tracking use cases. The production plan is to add
   `CONFIG_CELL_LOCATION_INTERVAL_SEC=28800` (8 hours).

2. **Cache the last known position** — the Lambda stores results in AWS
   Location Tracker. The dashboard always reads from Tracker, not HERE,
   so repeated dashboard views cost nothing.

3. **Only resolve when the device moves** — a future enhancement could
   compare `_loc_cell_id` with the previously known cell and skip the HERE
   call if the device hasn't changed towers.

4. **`fallback=area`** — already set in the Lambda. This means even if
   HERE doesn't have the exact cell in its database, it returns a
   coarser eNodeB-level estimate rather than an error, avoiding wasted
   retries.

---

## Firmware Configuration Reference

Key options in `prj.conf`:

```kconfig
# Core SDK
CONFIG_CONEXIO_CLOUD=y
CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=30      # Telemetry interval (s)

# PSM — keep modem in µA sleep between publishes
CONFIG_CONEXIO_CLOUD_PSM=y
CONFIG_CONEXIO_CLOUD_PSM_TAU_SEC=7200     # Network keepalive every 2h
CONFIG_CONEXIO_CLOUD_PSM_ACTIVE_TIME_SEC=30

# Offline buffer — 100 samples × 60s = ~100 min coverage
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE=100

# FOTA
CONFIG_CONEXIO_CLOUD_FOTA=y

# Retry with exponential backoff (5s → 300s, reboot after 10 failures)
CONFIG_CONEXIO_CLOUD_RETRY=y
CONFIG_CONEXIO_CLOUD_RETRY_BASE_SEC=5
CONFIG_CONEXIO_CLOUD_RETRY_MAX_SEC=300
CONFIG_CONEXIO_CLOUD_RETRY_MAX_ATTEMPTS=10

# Battery (nPM1300 fuel gauge)
CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=n      # Disable modem ADC path
CONFIG_CONEXIO_CLOUD_BATTERY_METRICS=y   # Enable SOC + drain rate metrics

# Cellular location
CONFIG_CELL_LOCATION=y                   # Enable AT%NCELLMEAS module
```

---

## Build Variants

### Development (default)

Uses `prj.conf` only. UART console enabled, log level INF.

```bash
west build -b conexio_stratus_pro/nrf9151/ns
```

### Debug

Adds DBG-level logging and assertions.

```bash
west build -b conexio_stratus_pro/nrf9151/ns -- \
  -DEXTRA_CONF_FILE=debug.conf
```

### Production / Low-Power

Disables UART console and logging. Reduces current draw.

```bash
west build -b conexio_stratus_pro/nrf9151/ns -- \
  -DEXTRA_CONF_FILE=low_power.conf
```

> **Simulated sensors warning:** `CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=y`
> is on by default. The build emits a `#warning` as a reminder. Set it to `n`
> and implement real sensor reads before any production deployment.

---

## Commands and Settings

### Built-in SDK Commands (automatic)

| Command | Payload | Effect |
|---------|---------|--------|
| `REBOOT` | `{}` | Immediate device reboot |
| `SET_INTERVAL` | `{"intervalSec": 60}` | Change telemetry interval |
| `FIRMWARE_UPDATE` | `{"url": "..."}` | Trigger FOTA download |

### Application Commands

| Command | Payload | Effect |
|---------|---------|--------|
| `FAN_ON` | `{"speed": 80}` (optional) | Turn on fan / actuator |
| `FAN_OFF` | `{}` | Turn off fan / actuator |
| `LED_ON` | `{}` | Turn on board LED (gpio0 pin 25) |
| `LED_OFF` | `{}` | Turn off board LED |

`LED_ON` / `LED_OFF` are wired to the Device Schedules feature — use them
to test timed command delivery from the Conexio Console. The SDK stores the
`stopAt` time in NVS so `LED_OFF` fires even if the device loses connectivity
after receiving `LED_ON`.

### Settings (OTA Config)

| Key | Type | Range | Default | Effect |
|-----|------|-------|---------|--------|
| `telemetryIntervalSec` | int | 10–604800 | 30 | Publish interval (built-in) |
| `alertThreshold` | int | 0–200 | 80 | App-level alert trigger |
| `debugMode` | bool | — | false | App-level debug flag |

---

## Battery Metrics (nPM1300)

The Stratus Pro uses the nPM1300 PMIC with built-in fuel gauge. Three
battery metrics are published on every telemetry cycle:

| Metric | Description |
|--------|-------------|
| `_battery_mv` | Battery terminal voltage (mV) — read from `SENSOR_CHAN_GAUGE_VOLTAGE` |
| `_battery_soc_pct` | State of charge (0.0–100.0%) from nRF Fuel Gauge library |
| `_battery_drain_pct_hr` | Average discharge rate (%/hour) — published only during active discharge |

The modem AT%XVBAT path is disabled (`CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=n`)
because the nPM1300 provides a more accurate reading directly at the battery
terminals.

---

## FOTA — Firmware Over-the-Air

FOTA is enabled via `CONFIG_CONEXIO_CLOUD_FOTA=y`. The SDK handles:

1. Cloud delivers `FIRMWARE_UPDATE` command with a signed download URL
2. SDK downloads the binary over HTTPS using the nRF9151 modem
3. MCUboot applies the update on next reboot
4. `_app_fw_version` in the next telemetry confirms the new version

**URL length:** Conexio Cloud firmware URLs contain a large query string
(~1,700 chars). `CONFIG_DOWNLOADER_MAX_FILENAME_SIZE=2048` is required to
prevent the URL from being truncated, which would cause an HTTP 403.

See `FOTA_BUILD_REFERENCE.md` in this directory for full build and signing
instructions.

---

## Power Management

### PSM (Power Saving Mode)

The modem enters µA-level sleep between transmissions.

```
TAU timer  (T3412) = 7200s = 2h  → network keepalive interval
Active time (T3324) = 30s        → window to connect + publish + sleep
```

The actual negotiated values are reported back by the network and published
as `_psm_tau_sec` and `_psm_active_sec` on boot.

### Main Loop Sleep

The main loop sleeps in 5-second increments. This lets `SET_INTERVAL` or
`telemetryIntervalSec` OTA Config changes take effect within 5 seconds
rather than waiting out the full current interval.

### Typical Current Profile (60s interval, PSM enabled)

| Phase | Duration | Current |
|-------|----------|---------|
| LTE attach + publish | ~10–15s | ~70–100 mA |
| AT%NCELLMEAS (if enabled) | ~1–5s | ~30–50 mA |
| PSM sleep | remainder of 60s | ~2–5 µA |

---

## Offline Buffering

When the device loses LTE connectivity, telemetry is stored in NVS flash:

- Buffer size: 100 entries (`CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE`)
- On reconnect: replays in batches of 10 (`CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH`)
- At 60s interval: ~100 minutes of offline coverage before oldest entries
  are overwritten
- Each buffered entry includes the original timestamp so the cloud can
  reconstruct the correct timeline

---

*Copyright (c) 2026 Conexio Technologies, Inc. — Apache-2.0 License*
