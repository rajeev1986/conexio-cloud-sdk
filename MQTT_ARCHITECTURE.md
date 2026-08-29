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

## Device → Cloud (D2C)

Data is split across **five separate topics** based on content type. This allows
the cloud to apply different IoT Rules per data type — e.g. location metrics trigger
the HERE/AWS Location Lambda; alerts trigger SNS; diagnostics go to a metrics store.

Each publish includes a `seq` field (monotonically increasing per topic, per boot)
so the cloud can detect gaps in any stream independently.

### 1. Telemetry — app sensor data

```
Topic:   v1/devices/{deviceId}/telemetry
QoS:     1
When:    Every interval when sensor callbacks are registered or app metrics queued
Retain:  No
```

Payload:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "seq":        42,
  "topic":     "telemetry",
  "metrics": {
    "temperature": 29.5,
    "humidity":    71.0,
    "_battery_mv": 4046.99
  }
}
```

Contains: registered sensor callbacks + `conexio_cloud_send_metric()` values
(excluding `_loc_*` metrics which go to the location topic).

---

### 2. Diagnostics — SDK system health

```
Topic:   v1/devices/{deviceId}/diagnostics
QoS:     1
When:    Every publish interval (always has data from SDK auto-metrics)
Retain:  No
```

Payload:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "seq":        42,
  "topic":     "diagnostics",
  "metrics": {
    "_rssi":              -72,
    "_snr":               15,
    "_session_id":        "a3f2c891",
    "_reboot_cnt":        4,
    "_reboot_reason":     "por",
    "_sdk_version":       "2.3.0",
    "_app_fw_version":    "1.0.7",
    "_modem_fw":          "nrf91x1_2.0.4",
    "_operator":          "VZW",
    "_lte_mode":          7,
    "_lte_band":          13,
    "_cell_id":           129061889,
    "_tac":               52228,
    "_psm_tau_sec":       7200,
    "_psm_active_sec":    30,
    "_lte_connect_ms":    6200,
    "_conn_loss":         0,
    "_modem_temp":        26,
    "_tx_kb":             4,
    "_rx_kb":             5,
    "_battery_soc_pct":   85.4,
    "_battery_drain_pct_hr": 0.42,
    "_publish_success_count": 12
  }
}
```

Contains: all SDK auto-metrics — signal quality, reboot tracking, LTE parameters,
PSM/eDRX grants, data usage counters, battery fuel gauge metrics.

Delta encoding: `_lte_band`, `_cell_id`, `_tac` only emitted when value changes
(+ on boot + every `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` as heartbeat).

---

### 3. Location — cellular position fixes

```
Topic:   v1/devices/{deviceId}/location
QoS:     1
When:    Immediately after AT%NCELLMEAS completes (CONFIG_CELL_LOCATION_PUBLISH_ON_FIX=y)
Retain:  No
```

Payload:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "seq":        7,
  "topic":     "location",
  "metrics": {
    "_loc_mcc":        311,
    "_loc_mnc":        480,
    "_loc_cell_id":    129061889,
    "_loc_tac":        52228,
    "_loc_earfcn":     5230,
    "_loc_rsrp":       -91,
    "_loc_timing_adv": 16,
    "_loc_neighbors":  "[{\"earfcn\":5230,\"pci\":198,\"rsrp\":-99}]"
  }
}
```

Contains: all metrics prefixed with `_loc_` — automatically routed to this topic
by the SDK when queued via `conexio_cloud_send_metric("_loc_*", ...)`.

The AWS IoT Rule for this topic triggers the `iot-dashboard-location` Lambda
which calls the HERE Positioning API and updates the AWS Location Tracker.

---

### 4. Logs — firmware log stream

```
Topic:   v1/devices/{deviceId}/logs
QoS:     1
When:    Every interval when CONFIG_CONEXIO_CLOUD_LOG_STREAM=y and entries pending
Retain:  No
```

Payload:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "seq":        3,
  "topic":     "logs",
  "metrics": {
    "_log": [
      {"l":"WRN","m":"cell_location","s":"neighbors=0, accuracy may be low"},
      {"l":"ERR","m":"fota","s":"download timeout after 300s"}
    ]
  }
}
```

Contains: log entries captured by the Zephyr log backend (`LOG_BACKEND_DEFINE`).
Level abbreviations: `DBG` / `INF` / `WRN` / `ERR`. Module name truncated to 16 chars.
Only published when `CONFIG_CONEXIO_CLOUD_LOG_STREAM=y` and log entries are pending.

---

### 5. Alerts — app-triggered threshold breaches

```
Topic:   v1/devices/{deviceId}/alerts
QoS:     1
When:    On demand — called by application via conexio_cloud_publish_alert()
Retain:  No
```

Payload:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "metric":    "temperature",
  "value":     87.3,
  "threshold": 85.0
}
```

Published immediately to a dedicated topic so the cloud can apply a separate
IoT Rule — e.g. SNS notification within seconds, independently of the regular
DynamoDB telemetry write path.

**Application usage:**
```c
float temp = read_temperature();
if (temp > TEMP_ALERT_THRESHOLD) {
    conexio_cloud_publish_alert("temperature", temp, TEMP_ALERT_THRESHOLD);
}
```

---

### 6. Command ACK

```
Topic:   v1/devices/{deviceId}/commands/ack
QoS:     1 (queued for reliable delivery — retried if connection drops)
When:    After executing a command. Retried up to CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX times.
Retain:  No
```

Payload:

```json
{"commandId": "cmd_abc123", "sk": "CMD#1234567890", "result": "executed"}
```

**Reliable delivery:** ACKs are stored in a RAM retry queue and retried on each
`transport_poll()` cycle until delivered or the retry limit is reached. This
prevents the "delivered forever" dashboard state when the connection drops
between PUBACK and ACK publish.

---

### 7. Config ACK

```
Topic:   v1/devices/{deviceId}/config/ack
QoS:     1 (queued for reliable delivery)
When:    After applying OTA Config push — sent AFTER all setting handlers run
Retain:  No
```

Payload:

```json
{"configId": "cfg_xyz789", "success": true}
```

Sent after all setting handlers complete so it reports the actual result
(applied/failed), not a premature acknowledgement.

---

### 8. FOTA Status Updates (AWS IoT Jobs — no version prefix)

```
Topic:   $aws/things/{deviceId}/jobs/{jobId}/update
QoS:     1
When:    During and after firmware download job
Retain:  No
```

Payload examples:

```json
{"status":"IN_PROGRESS","statusDetails":{"step":"downloading","progress":"42"}}
{"status":"IN_PROGRESS","statusDetails":{"step":"installing"}}
{"status":"SUCCEEDED"}
{"status":"FAILED","statusDetails":{"reason":"download_error"}}
```

This uses the native AWS IoT Jobs API — the format is fixed by AWS and cannot
be changed. No version prefix is used. EventBridge triggers the
`iot-dashboard-firmware-job-status` Lambda on status change.

---

## Cloud → Device (C2D)

Subscriptions use the versioned topic prefix. Subscribed on every CONNACK.

### 1. Commands

```
Topic:   v1/devices/{deviceId}/commands
QoS:     1
Source:  Conexio Console — Commands page, Schedules page
```

Payload:

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

Built-in commands (handled automatically by SDK):

| Command | Effect |
|---|---|
| `REBOOT` | `sys_reboot(SYS_REBOOT_COLD)` after 500ms log flush |
| `SET_INTERVAL` | Updates telemetry interval at runtime |
| `FIRMWARE_UPDATE` | Starts FOTA download (respects `fota_set_can_start_cb()`) |

### 2. OTA Config

```
Topic:   v1/devices/{deviceId}/config
QoS:     1
Source:  Conexio Console — OTA Config page
```

Payload:

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

v1/.../telemetry ──────────────────► IoT Rule ─────────────► ingestion Lambda → DynamoDB
v1/.../diagnostics ────────────────► IoT Rule ─────────────► metrics Lambda   → DynamoDB
v1/.../location ───────────────────► IoT Rule ─────────────► location Lambda → HERE API → Tracker
v1/.../logs ───────────────────────► IoT Rule ─────────────► logs Lambda      → CloudWatch / S3
v1/.../alerts ─────────────────────► IoT Rule ─────────────► SNS notification (immediate)
                                                               └─► DynamoDB (alert log)

v1/.../commands/ack ───────────────► (rule optional)        → dashboard WebSocket
v1/.../config/ack ─────────────────► (rule optional)        → dashboard WebSocket

               ◄── v1/.../commands ─── commands Lambda ◄─── dashboard action
v1/.../commands/ack ───────────────►   (queued retry)

               ◄── v1/.../config ───── config Lambda ◄───── OTA Config page
v1/.../config/ack ─────────────────►   (queued retry)

               ◄── FIRMWARE_UPDATE (via v1/.../commands)
$aws/.../jobs/update ──────────────► EventBridge ──────────► firmware-job-status Lambda
                                                              └─► WebSocket (Completed/Failed)
```

---

## Critical Message Ordering (incoming commands)

For every incoming QoS 1 message the SDK executes in strict order to prevent
re-delivery loops (e.g. a `REBOOT` command rebooting before its PUBACK is sent):

```
1. mqtt_publish_qos1_ack()     — PUBACK to broker (AWS stops retrying)
2. transport_queue_ack()        — queue dashboard ACK for reliable delivery
3. k_sleep(200ms)               — flush window: PUBACK bytes leave modem
4. transport_on_message()       — dispatch to app handler (safe to reboot now)
```

Config ACK is sent via `transport_config_ack()` after step 4 completes, so it
reflects the real success/failure from setting handlers.

---

## ACK Reliability

Both `commands/ack` and `config/ack` are queued in a RAM retry buffer
(`ACK_QUEUE_DEPTH = 8`) rather than fire-and-forget. On each `transport_poll()`
cycle, `transport_drain_ack_queue()` attempts delivery of all pending ACKs.

- On success: slot freed immediately
- On failure: retry counter incremented; slot dropped after `CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX` (default 5) retries
- Prevents "delivered forever" dashboard state on transient disconnects

---

## Kconfig Reference

| Option | Default | Description |
|---|---|---|
| `CONFIG_CONEXIO_CLOUD_TOPIC_VERSION` | `"v1"` | Topic version prefix |
| `CONFIG_CONEXIO_CLOUD_ACK_RETRY_MAX` | `5` | ACK retry limit before dropping |
| `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` | `10` | Diagnostics heartbeat interval (publishes) |
| `CONFIG_CONEXIO_CLOUD_LOG_STREAM` | `n` | Enable Zephyr log backend forwarding |
| `CONFIG_CONEXIO_CLOUD_LOG_LEVEL` | `2` | Min log level (0=DBG…3=ERR) |
| `CONFIG_CELL_LOCATION_PUBLISH_ON_FIX` | `y` | Publish location topic immediately on AT%NCELLMEAS |

---

## Source Files

| File | Purpose |
|---|---|
| `src/transport/mqtt_transport.c` | MQTT client, versioned topic strings, ACK retry queue |
| `src/transport.h` | Internal transport interface, TOPIC_CAT_* constants |
| `src/conexio_cloud.c` | Payload builder per category, metric routing, sequence numbers |
| `src/fota.c` | AWS IoT Jobs status (`$aws/things/.../jobs/.../update`) |
| `src/log_stream.c` | Zephyr log backend → logs topic |
| `src/cell_location.c` | AT%NCELLMEAS → location topic |
| `include/conexio_cloud/conexio_cloud.h` | Public API |
| `Kconfig` | All configurable parameters |

---

*Last updated: August 2026 — Conexio SDK v2.3.0 / App v1.0.7*
