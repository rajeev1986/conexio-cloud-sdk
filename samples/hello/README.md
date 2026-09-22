# Hello — Minimal Conexio Cloud Sample

The simplest possible Conexio Cloud SDK application. Connects the
Conexio Stratus Pro to the Conexio Cloud platform and publishes a
single metric (`uptime_sec`) every 60 seconds.

Start here to verify your hardware, certificates, and cloud pipeline
before adding sensors, commands, or settings.

---

## What this sample does

1. Calls `conexio_cloud_init()` — handles LTE attach, NTP, TLS, MQTT
2. Registers one sensor callback: `uptime_sec` (seconds since boot)
3. Connects to MQTT and does an immediate boot publish
4. The SDK background thread publishes every `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC`

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD` | `y` | Enable the SDK |
| `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` | `60` | Publish every 60 seconds |
| `CONFIG_CONEXIO_CLOUD_CBOR` | `y` | CBOR binary encoding (50–70% smaller than JSON) |
| `CONFIG_CONEXIO_CLOUD_RETRY` | `y` | Exponential backoff reconnect |

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash
west espressif monitor   # or: screen /dev/tty.usbmodem* 115200
```

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Hello Sample ===
[00:00:00.257] <inf> app: App v1.0.0 | SDK v2.3.0
[00:00:02.783] <inf> conexio_lte: Connecting to LTE...
[00:00:09.771] <inf> conexio_lte: LTE registered (home) in 6988 ms
[00:00:09.817] <inf> conexio_cloud: NTP synced
[00:00:19.533] <inf> mqtt_transport: MQTT connected to mqtt.cloud.conexiotech.com
[00:00:19.787] <inf> app: Connected to Conexio Cloud — device: 355025934980275 | SDK v2.3.0
[00:00:21.900] <inf> app: Telemetry published
```

---

## Cloud JSON payload

```json
{
  "dev_id": "355025934980275",
  "ts": "2026-09-20T14:00:00.000Z",
  "metrics": {
    "uptime_sec": 21,
    "_rssi": 57,
    "_snr": 9,
    "_reboot_cnt": 1,
    "_sdk": "2.3.0",
    "_fw_ver": "1.0.0",
    "_lte_connect_ms": 6988,
    "_mfw": "mfw_nrf91x1_2.0.4"
  }
}
```

> The CBOR wire payload is binary — the Conexio Cloud backend decodes it
> to JSON automatically before storing and displaying.
