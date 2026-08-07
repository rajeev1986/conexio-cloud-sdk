# Conexio Console — Device Firmware

nRF Connect SDK v3.2.1 firmware samples for connecting devices to the Conexio Console
cloud dashboard.

## Samples overview

| Sample | Protocol | Commands | FOTA | Best for |
|--------|----------|----------|------|----------|
| `sdk-sample-app/` | MQTT or HTTP | ✅ | ❌ | Clean SDK integration reference |
| `sdk-sample-app-advanced/` | MQTT | ✅ | ✅ | Production devices (PSM + buffer + retry) |
| `fleet-provisioning/` | MQTT | ✅ | ❌ | Zero-touch production fleets |
| `mqtt/` | MQTT | ✅ | ❌ | Manual single-device / development |
| `http/` | HTTPS POST | ✅ (polled) | ❌ | Battery/sleepy HTTP-only sensors |

### Which sample should I use?

**`sdk-sample-app-advanced/` — recommended for production**
Uses the Conexio Cloud SDK with PSM power management, NVS offline buffering, FOTA
via AWS IoT Jobs + MCUboot, and exponential backoff retry with hardware watchdog.
The SDK handles all cloud protocol details automatically.

**`sdk-sample-app/` — SDK integration reference**
Clean, minimal example of the full SDK API. Start here to understand the programming
model before customising for your hardware.

**`fleet-provisioning/` — zero-touch fleet deployment**
One binary for your entire fleet. Each device provisions itself on first boot using
a shared claim certificate, then operates normally. No per-device configuration.

**`mqtt/` — manual single-device development**
Requires a manually created certificate per device. Use for development or one-off
devices.

**`http/` — telemetry + polled commands**
No device certificate — just an API key. After each telemetry send the device polls
`GET /v1/device-commands` and ACKs executed commands. Suitable for sleepy sensors
that wake periodically.

## Supported hardware

All samples target the **nRF91xx** series:
- nRF9160 DK (`nrf9160dk/nrf9160/ns`)
- Thingy:91 (`thingy91/nrf9160/ns`)
- Any nRF91xx-based custom board

## Cloud protocol conventions

### Telemetry payload

All samples send this JSON format to `devices/<id>/telemetry` (MQTT) or
`POST /v1/ingest` (HTTP):
```json
{
  "deviceId": "351358815179730",
  "timestamp": "2026-06-08T14:30:00.000Z",
  "metrics": {
    "temperature": 22.5,
    "humidity": 61.0,
    "_rssi": -72,
    "_reboot_cnt": 4
  }
}
```

### Special metrics

| Metric | Source | Dashboard page |
|--------|--------|----------------|
| `_rssi` | Modem RSRP, auto-added by SDK | Fleet Health → Device Signal |
| `_snr` | Optional, add manually | Fleet Health → Device Signal |
| `_reboot_cnt` | NVS counter, auto-added by SDK | Fleet Health → Reboot Tracking |

### Command format

Commands arrive on `devices/<id>/commands` (MQTT) or via `GET /v1/device-commands`
(HTTP poll) as:
```json
{
  "type": "command",
  "command": "FAN_ON",
  "commandId": "cmd-abc123",
  "sk": "CMD#2026-06-08T...",
  "payload": { "speed": 80 }
}
```

After executing, publish an ACK to `devices/<id>/commands/ack`:
```json
{ "commandId": "cmd-abc123", "sk": "CMD#...", "result": "fan_on" }
```

The SDK handles ACK publishing automatically. Manual samples (`mqtt/`,
`fleet-provisioning/`) also publish ACKs.

### OTA Config format

Config pushes arrive on `devices/<id>/commands` as:
```json
{
  "type": "config",
  "version": 3,
  "configId": "cfg-xyz789",
  "config": { "telemetryIntervalSec": 120, "debugMode": false }
}
```

After applying, publish an ACK to `devices/<id>/config/ack`:
```json
{ "configId": "cfg-xyz789", "success": true }
```

### FOTA job document

Firmware jobs created from the Firmware page produce this document:
```json
{
  "operation": "firmware_update",
  "firmwareVersion": "1.4.2",
  "location": { "url": "https://...s3.amazonaws.com/firmware/.../fw.bin" },
  "checksum": "sha256:abc123...",
  "fileSize": 131072
}
```

Parse `location.url` (not `firmware.url`) for the download URL.

### SET_INTERVAL command

The `interval` payload value is in **seconds** across all samples:
```json
{ "command": "SET_INTERVAL", "payload": { "interval": 120 } }
```

### HTTP response codes

`POST /v1/ingest` returns **202 Accepted** on success (not 200 or 201). Treat
200, 201, and 202 all as success.

## Before you build

1. Install nRF Connect SDK v3.2.1 via nRF Connect for Desktop
2. Deploy the Conexio Console backend: `./deploy.sh --no-vpc` (from repo root)
3. Get your AWS IoT endpoint:
   ```bash
   aws iot describe-endpoint --endpoint-type iot:Data-ATS
   # e.g. abc123xyz.iot.us-east-1.amazonaws.com
   ```
4. Read the README in each sample subdirectory for sample-specific setup

## SDK vs. manual samples

The SDK samples (`sdk-sample-app`, `sdk-sample-app-advanced`) hide all protocol
details. The manual samples (`mqtt`, `http`, `fleet-provisioning`) show the raw
protocol for reference and give full control.

For new projects, use the SDK samples. Use the manual samples as protocol
documentation or when you need direct MQTT/HTTP access without the SDK abstraction.
