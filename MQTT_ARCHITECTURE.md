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
| Keepalive | 120 seconds (configurable via `CONFIG_CONEXIO_CLOUD_MQTT_KEEPALIVE_SEC`) |
| Port | 8883 |

### TLS Security Tags (modem credential storage)

| Tag | Default | Content |
|---|---|---|
| `CONFIG_CONEXIO_CLOUD_CA_TAG` | 100 | AWS Root CA (AmazonRootCA1.pem) |
| `CONFIG_CONEXIO_CLOUD_CERT_TAG` | 101 | Device certificate (written by fleet-provisioning) |
| `CONFIG_CONEXIO_CLOUD_KEY_TAG` | 102 | Device private key (written by fleet-provisioning) |

Persistent session (`clean_session = 0`) means the AWS IoT Core broker
queues QoS 1 messages addressed to this device while it sleeps in PSM.
They are delivered immediately on the next reconnect — no commands are missed
during deep sleep windows.

---

## Device → Cloud (D2C)

### 1. Telemetry

```
Topic:   devices/{deviceId}/telemetry
QoS:     1 (AT_LEAST_ONCE)
When:    Every CONFIG_CONEXIO_CLOUD_INTERVAL_SEC seconds (default 30s)
Retain:  No
```

Payload — compact JSON, no whitespace:

```json
{
  "deviceId":  "355025934980275",
  "timestamp": "2026-08-25T12:34:56.789Z",
  "metrics": {
    "_rssi":              -72,
    "_snr":               15,
    "_session_id":        "a3f2c891",
    "_reboot_cnt":        4,
    "_reboot_reason":     "por",
    "_sdk_version":       "2.2.0",
    "_app_fw_version":    "1.0.6",
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
    "_battery_mv":        4046.99,
    "_battery_soc_pct":   85.4,
    "_battery_drain_pct_hr": 0.42,
    "_publish_success_count": 12,
    "_loc_mcc":           311,
    "_loc_mnc":           480,
    "_loc_cell_id":       129061889,
    "_loc_tac":           52228,
    "_loc_earfcn":        5230,
    "_loc_rsrp":          -91,
    "_loc_timing_adv":    16,
    "_loc_neighbors":     "[{\"earfcn\":5230,\"pci\":198,\"rsrp\":-99}]",
    "_log": [
      {"l":"WRN","m":"cell_location","s":"neighbors=0, accuracy may be low"}
    ],
    "temperature": 29.5,
    "humidity":    71.0
  }
}
```

**Metric publish tiers** (controls how often each metric appears):

| Tier | Metrics | Frequency |
|---|---|---|
| BOOT-ONCE | `_reboot_cnt`, `_reboot_reason`, `_sdk_version`, `_app_fw_version`, `_modem_fw`, `_operator`, `_lte_mode`, `_lte_connect_ms`, `_psm_tau_sec`, `_psm_active_sec`, `_edrx_ms`, `_session_id` | First publish after each boot only |
| DELTA | `_lte_band`, `_cell_id`, `_tac` | On boot + when value changes + every `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` publishes as heartbeat |
| EVERY | `_rssi`, `_snr`, `_conn_loss`, `_tx_kb`, `_rx_kb`, `_modem_temp`, `_battery_*`, application sensors | Every publish |
| ON-FIX | `_loc_*` metrics | Immediately after AT%NCELLMEAS completes (separate publish triggered by `k_work`) |
| ON-EVENT | `_log` array | When `CONFIG_CONEXIO_CLOUD_LOG_STREAM=y` and log entries are pending |

---

### 2. Command ACK

```
Topic:   devices/{deviceId}/commands/ack
QoS:     1
When:    After the device executes an incoming command
Retain:  No
```

Payload:

```json
{"commandId": "cmd_abc123", "sk": "CMD#1234567890", "result": "executed"}
```

---

### 3. Config ACK

```
Topic:   devices/{deviceId}/config/ack
QoS:     1
When:    After the device applies an OTA Config push — sent AFTER all setting
         handlers run so the result reflects actual success or failure
Retain:  No
```

Payload:

```json
{"configId": "cfg_xyz789", "success": true}
```

---

### 4. FOTA Status Updates

```
Topic:   $aws/things/{deviceId}/jobs/{jobId}/update
QoS:     1
When:    During and after a firmware download job
Retain:  No
```

Payload examples:

```json
{"status":"IN_PROGRESS","statusDetails":{"step":"downloading","progress":"42"}}
{"status":"IN_PROGRESS","statusDetails":{"step":"installing"}}
{"status":"SUCCEEDED"}
{"status":"FAILED","statusDetails":{"reason":"download_error"}}
```

This topic is the native AWS IoT Jobs API — the broker routes updates to
EventBridge, which triggers the `iot-dashboard-firmware-job-status` Lambda
to update DynamoDB and push the status change to the dashboard via WebSocket.

---

## Cloud → Device (C2D)

The device subscribes to two topics on every MQTT connect (CONNACK → SUBSCRIBE):

### 1. Commands

```
Topic:   devices/{deviceId}/commands
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

Built-in SDK commands (handled automatically, no app registration needed):

| Command | Effect |
|---|---|
| `REBOOT` | `sys_reboot(SYS_REBOOT_COLD)` after 500ms log flush |
| `SET_INTERVAL` | Updates telemetry interval at runtime |
| `FIRMWARE_UPDATE` | Starts FOTA download |

### 2. OTA Config

```
Topic:   devices/{deviceId}/config
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
Device (nRF9151)                 AWS IoT Core              Conexio Cloud
────────────────                 ────────────              ─────────────

telemetry ──────────────────────► IoT Rule ──────────────► ingestion Lambda
                                                            │
                                                            ├─► DynamoDB (devices, telemetry)
                                                            ├─► EventBridge (iot-dashboard-internal)
                                                            │     └─► location Lambda (→ HERE API)
                                                            └─► WebSocket push to dashboard

                  ◄── commands ────── commands Lambda ◄──── dashboard action
commands/ack ───────────────────►

                  ◄── config ──────── config Lambda ◄─────── OTA Config page push
config/ack ─────────────────────►

                  ◄── FIRMWARE_UPDATE (via commands topic)
$aws/.../jobs/update ───────────► EventBridge ────────────► firmware-job-status Lambda
                                                              └─► WebSocket (Completed/Failed)
```

---

## Critical Message Ordering (commands)

For every incoming QoS 1 message the SDK executes in strict order to prevent
re-delivery loops (e.g. a `REBOOT` command that reboots before its PUBACK is sent):

```
1. mqtt_publish_qos1_ack()   — PUBACK to broker (AWS stops retrying)
2. publish_command_ack()      — dashboard ACK (Command History: delivered → acknowledged)
3. k_sleep(200ms)             — flush window: ensures both packets leave the modem
4. transport_on_message()     — dispatch to app handler (safe to reboot now)
```

Config ACK is deferred until step 4 completes so it reports the real
success/failure from setting handlers, not a premature acknowledgement.

---

## Production Hardening — Known Gaps

The following items are not yet implemented and should be addressed before
large-scale fleet deployment:

### 1. No schema versioning in topic paths

**Current:** `devices/{deviceId}/telemetry`
**Recommended:** `devices/{deviceId}/v1/telemetry`

Adding a version prefix now costs nothing. Without it, changing the JSON
schema requires all devices and all Lambda consumers to update simultaneously
with no transition period.

### 2. ACK publish failures are silent

If the MQTT connection drops between the PUBACK and the dashboard ACK publish,
the dashboard shows "delivered" indefinitely with no retry. A small in-RAM
retry queue for `commands/ack` and `config/ack` (attempt on next publish cycle)
would close this gap.

### 3. All data on one telemetry topic

Telemetry metrics, location fixes, and log stream entries all multiplex onto
`devices/{deviceId}/telemetry`. As payload size grows this approaches AWS IoT
Core's 128 KB message limit. Consider splitting:

- `devices/{deviceId}/telemetry` — sensor + modem metrics (high frequency)
- `devices/{deviceId}/logs` — `_log` entries (when `LOG_STREAM=y`)
- Location data already uses a dedicated Lambda path via EventBridge

### 4. No device-initiated alert topic

Threshold breaches are currently embedded in the telemetry payload. A
dedicated `devices/{deviceId}/alerts` topic would allow different IoT Rules
for immediate notifications (SNS) vs. storage (DynamoDB), without
processing every telemetry message through the alert path.

---

## Source Files

| File | Purpose |
|---|---|
| `src/transport/mqtt_transport.c` | MQTT client, topic strings, pub/sub, TLS config |
| `src/conexio_cloud.c` | Payload builder, metric queue, session ID, boot sequence |
| `src/fota.c` | AWS IoT Jobs status updates (`$aws/things/.../jobs/.../update`) |
| `include/conexio_cloud/conexio_cloud.h` | Public API |
| `Kconfig` | All configurable parameters |

---

*Last updated: August 2026 — Conexio SDK v2.2.0*
