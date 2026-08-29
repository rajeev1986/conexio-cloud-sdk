# Conexio Cloud SDK — MQTT Topic Architecture

This document describes the complete MQTT topic structure between the
Conexio Stratus Pro device (nRF9151) and the Conexio Cloud backend.

---

## Connection Parameters

| Parameter | Value |
|---|---|
| Broker | `mqtt.cloud.conexiotech.com:8883` |
| Transport | MQTT over TLS 1.2 (AWS IoT Core) |
| Authentication | Mutual TLS — device certificate + private key |
| MQTT version | 3.1.1 |
| Session | Persistent (`clean_session = 0`) |
| Keepalive | 120 seconds (`CONFIG_CONEXIO_CLOUD_MQTT_KEEPALIVE_SEC`) |
| Port | 8883 |

### TLS Security Tags (modem credential storage)

| Tag | Default | Content |
|---|---|---|
| `CONFIG_CONEXIO_CLOUD_CA_TAG` | 100 | AWS Root CA (AmazonRootCA1.pem) |
| `CONFIG_CONEXIO_CLOUD_CERT_TAG` | 101 | Device certificate (written by fleet-provisioning) |
| `CONFIG_CONEXIO_CLOUD_KEY_TAG` | 102 | Device private key (written by fleet-provisioning) |

Persistent session (`clean_session = 0`) means the AWS IoT Core broker
queues QoS 1 messages addressed to this device while it sleeps in PSM.
They are delivered immediately on the next reconnect — no commands are missed.

---

## Topic Versioning

All D2C topics use a version prefix controlled by `CONFIG_CONEXIO_CLOUD_TOPIC_VERSION`
(default `v1`). Topics take the form:

```
{version}/devices/{deviceId}/{type}
```

The prefix allows the cloud to apply different IoT Rules per schema version.
A v2 device can coexist with v1 devices without changes to existing Rules — both
versions publish simultaneously and each is processed independently.

**To migrate to v2:** change `CONFIG_CONEXIO_CLOUD_TOPIC_VERSION="v2"` in `prj.conf`,
deploy to a subset of devices, and add v2 IoT Rules on the cloud side. No flag day.

---

## JSON Field Name Reference (SDK v2.3.0)

All payload field names were shortened in SDK v2.3.0 to reduce per-message byte overhead.
**~82 bytes saved per publish** (~118 KB/device/day at 60s interval).

### Envelope Fields (all topics)

| Field (v2.3.0) | Old name | Type | Description |
|---|---|---|---|
| `dev_id` | `deviceId` | string | 15-digit IMEI |
| `ts` | `timestamp` | string | ISO-8601 UTC timestamp |
| `seq` | *(new)* | number | Per-topic monotonic counter. Resets on reboot. |
| `topic` | *(new)* | string | Topic type: `telemetry`, `diagnostics`, `location`, `logs` |
| `metrics` | `metrics` | object | Key-value metric payload |

### Diagnostics Metrics (`v1/devices/{id}/diagnostics`)

| Field (v2.3.0) | Old name | Type | Tier | Description |
|---|---|---|---|---|
| `_rssi` | `_rssi` | number | EVERY | RSRP signal strength (dBm) |
| `_snr` | `_snr` | number | EVERY | Signal-to-noise ratio |
| `_conn_loss` | `_conn_loss` | number | EVERY | Connection loss count since boot |
| `_reset_loop` | `_reset_loop` | number | EVERY | 1 if modem detected reset loop |
| `_tx_kb` | `_tx_kb` | number | EVERY | KB transmitted this session |
| `_rx_kb` | `_rx_kb` | number | EVERY | KB received this session |
| `_modem_temp` | `_modem_temp` | number | EVERY | Modem die temperature (°C) |
| `_batt_soc` | `_battery_soc_pct` | number | EVERY | Battery state of charge (0–100%) |
| `_batt_drain_hr` | `_battery_drain_pct_hr` | number | EVERY | Drain rate (%/hr, discharge only) |
| `_pub_ok` | `_publish_success_count` | number | EVERY | Successful publish count since boot |
| `_session_id` | *(new in v2.2.0)* | string | BOOT-ONCE | Random 8-hex session run ID |
| `_reboot_cnt` | `_reboot_cnt` | number | BOOT-ONCE | NVS-persisted reboot counter |
| `_reboot_reason` | `_reboot_reason` | string | BOOT-ONCE | `por`, `pin`, `wdt`, `soft`, etc. |
| `_sdk` | `_sdk_version` | string | BOOT-ONCE | Conexio SDK version, e.g. `"2.3.0"` |
| `_fw_ver` | `_app_fw_version` | string | BOOT-ONCE | App firmware version, e.g. `"1.0.7"` |
| `_mfw` | `_modem_fw` | string | BOOT-ONCE | Modem firmware, e.g. `"nrf91x1_2.0.4"` |
| `_op` | `_operator` | string | BOOT-ONCE | Network operator name, e.g. `"VZW"` |
| `_lte_mode` | `_lte_mode` | number | BOOT-ONCE | 7=LTE-M, 9=NB-IoT |
| `_lte_connect_ms` | `_lte_connect_ms` | number | BOOT-ONCE | Time to LTE attach (ms) |
| `_psm_tau_sec` | `_psm_tau_sec` | number | BOOT-ONCE | Granted PSM TAU timer (s) |
| `_psm_active_sec` | `_psm_active_sec` | number | BOOT-ONCE | Granted PSM active window (s) |
| `_edrx_ms` | `_edrx_ms` | number | BOOT-ONCE | Granted eDRX interval (ms) |
| `_edrx_ptw_ms` | `_edrx_ptw_ms` | number | BOOT-ONCE | Granted eDRX paging time window (ms) |
| `_lte_band` | `_lte_band` | number | DELTA | Active LTE band (on change + heartbeat) |
| `_cell_id` | `_cell_id` | number | DELTA | E-UTRAN Cell ID (on change + heartbeat) |
| `_tac` | `_tac` | number | DELTA | Tracking Area Code (on change + heartbeat) |

**Tiers explained:**
- `BOOT-ONCE` — only in the first publish after each reboot
- `EVERY` — in every diagnostics publish
- `DELTA` — only when value changes, plus on boot and every `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` publishes as a heartbeat

### Telemetry Metrics (`v1/devices/{id}/telemetry`)

| Field (v2.3.0) | Old name | Type | Description |
|---|---|---|---|
| `temperature` | `temperature` | number | Application sensor (example) |
| `humidity` | `humidity` | number | Application sensor (example) |
| `_batt_mv` | `_battery_mv` | number | Battery voltage (mV) from nPM1300 |
| *any app metric* | *any app metric* | number/string/bool | Queued via `conexio_cloud_send_metric()` |

Application metrics use whatever name the app registers. Only `_loc_*` names are
reserved — they are automatically routed to the location topic.

### Location Metrics (`v1/devices/{id}/location`)

| Field (v2.3.0) | Old name | Type | Description |
|---|---|---|---|
| `_loc_mcc` | `_loc_mcc` | number | Mobile Country Code |
| `_loc_mnc` | `_loc_mnc` | number | Mobile Network Code |
| `_loc_cell_id` | `_loc_cell_id` | number | E-UTRAN Cell ID |
| `_loc_tac` | `_loc_tac` | number | Tracking Area Code |
| `_loc_earfcn` | `_loc_earfcn` | number | EARFCN frequency channel |
| `_loc_rsrp` | `_loc_rsrp` | number | RSRP signal strength (dBm) |
| `_loc_timing_adv` | `_loc_timing_adv` | number | Timing advance (distance proxy) |
| `_loc_nbrs` | `_loc_neighbors` | string | JSON array of neighbour cells |

`_loc_nbrs` format: `[{"earfcn":5230,"pci":198,"rsrp":-99},...]`
— `earfcn` and `pci` are required; `rsrp` is optional but improves HERE accuracy.

### Log Metrics (`v1/devices/{id}/logs`)

| Field | Type | Description |
|---|---|---|
| `_log` | array | Array of log entry objects |
| `_log[].l` | string | Level: `DBG`, `INF`, `WRN`, `ERR` |
| `_log[].m` | string | Module name (truncated to 16 chars) |
| `_log[].s` | string | Log message (truncated to `CONFIG_CONEXIO_LOG_MSG_LEN` chars) |

### Alerts Payload (`v1/devices/{id}/alerts`)

| Field (v2.3.0) | Type | Description |
|---|---|---|
| `dev_id` | string | Device IMEI |
| `ts` | string | ISO-8601 UTC timestamp |
| `metric` | string | Name of the metric that breached threshold |
| `value` | number | Measured value |
| `threshold` | number | Configured threshold that was exceeded |

---

## Device → Cloud (D2C) — Full Payload Examples

### 1. Telemetry

```
Topic:   v1/devices/{deviceId}/telemetry
QoS:     1
When:    Every interval when sensor callbacks are registered or app metrics queued
```

```json
{
  "dev_id":  "355025934980275",
  "ts":      "2026-08-25T12:34:56.789Z",
  "seq":     42,
  "topic":   "telemetry",
  "metrics": {
    "temperature": 29.5,
    "humidity":    71.0,
    "_batt_mv":    4046.99
  }
}
```

---

### 2. Diagnostics

```
Topic:   v1/devices/{deviceId}/diagnostics
QoS:     1
When:    Every publish interval
```

```json
{
  "dev_id":  "355025934980275",
  "ts":      "2026-08-25T12:34:56.789Z",
  "seq":     42,
  "topic":   "diagnostics",
  "metrics": {
    "_rssi":          -72,
    "_snr":           15,
    "_session_id":    "a3f2c891",
    "_reboot_cnt":    4,
    "_reboot_reason": "por",
    "_sdk":           "2.3.0",
    "_fw_ver":        "1.0.7",
    "_mfw":           "nrf91x1_2.0.4",
    "_op":            "VZW",
    "_lte_mode":      7,
    "_lte_band":      13,
    "_cell_id":       129061889,
    "_tac":           52228,
    "_psm_tau_sec":   7200,
    "_psm_active_sec":30,
    "_lte_connect_ms":6200,
    "_conn_loss":     0,
    "_modem_temp":    26,
    "_tx_kb":         4,
    "_rx_kb":         5,
    "_batt_soc":      85.4,
    "_batt_drain_hr": 0.42,
    "_pub_ok":        12
  }
}
```

---

### 3. Location

```
Topic:   v1/devices/{deviceId}/location
QoS:     1
When:    Immediately after AT%NCELLMEAS completes (CONFIG_CELL_LOCATION_PUBLISH_ON_FIX=y)
```

```json
{
  "dev_id":  "355025934980275",
  "ts":      "2026-08-25T12:34:56.789Z",
  "seq":     7,
  "topic":   "location",
  "metrics": {
    "_loc_mcc":        311,
    "_loc_mnc":        480,
    "_loc_cell_id":    129061889,
    "_loc_tac":        52228,
    "_loc_earfcn":     5230,
    "_loc_rsrp":       -91,
    "_loc_timing_adv": 16,
    "_loc_nbrs":       "[{\"earfcn\":5230,\"pci\":198,\"rsrp\":-99}]"
  }
}
```

---

### 4. Logs

```
Topic:   v1/devices/{deviceId}/logs
QoS:     1
When:    Every interval when CONFIG_CONEXIO_CLOUD_LOG_STREAM=y and entries pending
```

```json
{
  "dev_id":  "355025934980275",
  "ts":      "2026-08-25T12:34:56.789Z",
  "seq":     3,
  "topic":   "logs",
  "metrics": {
    "_log": [
      {"l":"WRN","m":"cell_location","s":"neighbors=0, accuracy may be low"},
      {"l":"ERR","m":"fota","s":"download timeout after 300s"}
    ]
  }
}
```

---

### 5. Alerts

```
Topic:   v1/devices/{deviceId}/alerts
QoS:     1
When:    On demand — via conexio_cloud_publish_alert()
```

```json
{
  "dev_id":    "355025934980275",
  "ts":        "2026-08-25T12:34:56.789Z",
  "metric":    "temperature",
  "value":     87.3,
  "threshold": 85.0
}
```

---

### 6. Command ACK

```
Topic:   v1/devices/{deviceId}/commands/ack
QoS:     1 (queued — retried up to CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX times)
When:    After executing a command
```

```json
{"commandId": "cmd_abc123", "sk": "CMD#1234567890", "result": "executed"}
```

---

### 7. Config ACK

```
Topic:   v1/devices/{deviceId}/config/ack
QoS:     1 (queued — sent AFTER all setting handlers run)
When:    After applying OTA Config push
```

```json
{"configId": "cfg_xyz789", "success": true}
```

---

### 8. FOTA Status (AWS IoT Jobs — no version prefix, AWS-owned format)

```
Topic:   $aws/things/{deviceId}/jobs/{jobId}/update
QoS:     1
```

```json
{"status":"IN_PROGRESS","statusDetails":{"step":"downloading","progress":"42"}}
{"status":"SUCCEEDED"}
{"status":"FAILED","statusDetails":{"reason":"download_error"}}
```

---

## Cloud → Device (C2D)

### Commands

```
Topic:   v1/devices/{deviceId}/commands
QoS:     1
```

```json
{
  "type":      "command",
  "commandId": "cmd_abc123",
  "sk":        "CMD#1234567890",
  "command":   "FAN_ON",
  "payload":   "{\"speed\":80}",
  "stopAt":    "2026-08-25T14:00:00.000Z"
}
```

Built-in commands:

| Command | Effect |
|---|---|
| `REBOOT` | `sys_reboot(SYS_REBOOT_COLD)` after 500ms flush |
| `SET_INTERVAL` | Updates publish interval at runtime |
| `FIRMWARE_UPDATE` | Starts FOTA (respects `fota_set_can_start_cb()`) |

### OTA Config

```
Topic:   v1/devices/{deviceId}/config
QoS:     1
```

```json
{
  "type":     "config",
  "configId": "cfg_xyz789",
  "settings": {
    "telemetryIntervalSec": 60,
    "alertThreshold":       80,
    "debugMode":            false
  }
}
```

---

## Message Flow Diagram

```
Device (nRF9151)                    AWS IoT Core             Conexio Cloud
────────────────                    ────────────             ─────────────

v1/.../telemetry ──────────────────► v1_device_telemetry ──► ingestion Lambda → DynamoDB
v1/.../diagnostics ────────────────► v1_device_diagnostics ► ingestion Lambda → DynamoDB
v1/.../location ───────────────────► v1_device_location ───► location Lambda → HERE API → Tracker
v1/.../logs ───────────────────────► (rule optional) ────────► CloudWatch Logs / S3
v1/.../alerts ─────────────────────► v1_device_alerts ──────► alerts Lambda → DynamoDB + WebSocket
                                                               └─► SNS (if SNS_ALERT_TOPIC_ARN set)

v1/.../commands/ack ───────────────► (ACK retry queue)       → dashboard WebSocket
v1/.../config/ack ─────────────────► (ACK retry queue)       → dashboard WebSocket

               ◄── v1/.../commands ─── commands Lambda ◄─── dashboard action
               ◄── v1/.../config ───── config Lambda ◄───── OTA Config page

$aws/.../jobs/update ──────────────► EventBridge ──────────► firmware-job-status Lambda
                                                              └─► WebSocket (Completed/Failed)
```

---

## Critical Message Ordering (incoming commands)

```
1. mqtt_publish_qos1_ack()   — PUBACK to broker (AWS stops retrying)
2. transport_queue_ack()      — queue dashboard ACK for reliable delivery
3. k_sleep(200ms)             — flush: PUBACK bytes leave modem before any reboot
4. transport_on_message()     — dispatch to app handler (safe to reboot now)
```

Config ACK is sent via `transport_config_ack()` after step 4 — reflects real handler result.

---

## ACK Reliability

Both `commands/ack` and `config/ack` use a RAM retry queue (`ACK_QUEUE_DEPTH = 8`).
Drained on every `transport_poll()` cycle.

| Outcome | Action |
|---|---|
| Publish succeeds | Slot freed immediately |
| Publish fails | Retry count incremented |
| Retry limit reached | Slot dropped with warning (default: 5 retries) |

Prevents "delivered forever" dashboard state on transient disconnects.

---

## AWS IoT Rules (v1 — deployed)

| Rule name | Topic filter | Target Lambda |
|---|---|---|
| `v1_device_telemetry` | `v1/devices/+/telemetry` | `iot-dashboard-ingestion-processor` |
| `v1_device_diagnostics` | `v1/devices/+/diagnostics` | `iot-dashboard-ingestion-processor` |
| `v1_device_location` | `v1/devices/+/location` | `iot-dashboard-location` |
| `v1_device_alerts` | `v1/devices/+/alerts` | `iot-dashboard-alerts` |

Old rule `devices/+/telemetry` remains active for backwards compatibility
until all devices are upgraded to SDK v2.3.0.

---

## Kconfig Reference

| Option | Default | Description |
|---|---|---|
| `CONFIG_CONEXIO_CLOUD_TOPIC_VERSION` | `"v1"` | Topic version prefix |
| `CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX` | `5` | ACK retry limit before dropping |
| `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` | `10` | Diagnostics DELTA heartbeat interval |
| `CONFIG_CONEXIO_CLOUD_LOG_STREAM` | `n` | Enable Zephyr log backend forwarding |
| `CONFIG_CONEXIO_CLOUD_LOG_LEVEL` | `2` | Min log level (0=DBG…3=ERR) |
| `CONFIG_CONEXIO_LOG_MSG_LEN` | `80` | Max chars per log message |
| `CONFIG_CELL_LOCATION_PUBLISH_ON_FIX` | `y` | Publish location topic on AT%NCELLMEAS |
| `CONFIG_CELL_LOCATION_INTERVAL_SEC` | `28800` | Location fix interval (s) |
| `CONFIG_CELL_LOCATION_SEARCH_TYPE` | `1` | 0=default, 1=extended_light, 2=extended |

---

## Source Files

| File | Purpose |
|---|---|
| `src/transport/mqtt_transport.c` | MQTT client, versioned topic strings, ACK retry queue |
| `src/transport.h` | Internal transport interface, `TOPIC_CAT_*` constants |
| `src/conexio_cloud.c` | Payload builder per category, metric routing, sequence numbers |
| `src/fota.c` | AWS IoT Jobs status (`$aws/things/.../jobs/.../update`) |
| `src/log_stream.c` | Zephyr log backend → logs topic |
| `src/cell_location.c` | AT%NCELLMEAS → location topic |
| `include/conexio_cloud/conexio_cloud.h` | Public API |
| `Kconfig` | All configurable parameters |

---

*Last updated: August 2026 — Conexio SDK v2.3.0 / App v1.0.7*
