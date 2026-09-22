# Device Commands — Handling Cloud Commands

Demonstrates how to handle commands sent from the Conexio Console to
the device over MQTT (QoS 1). Commands are dispatched by the SDK to
your registered handler functions.

---

## What this sample does

- Registers three application commands: `LED_ON`, `LED_OFF`, `BUZZ`
- Built-in SDK commands (`REBOOT`, `SET_INTERVAL`, `FIRMWARE_UPDATE`) are
  available automatically — no registration needed
- Logs every received command to the serial console

---

## Built-in SDK commands (always available)

| Command | Payload | Effect |
|---------|---------|--------|
| `REBOOT` | `{}` | Immediately reboots the device |
| `SET_INTERVAL` | `{"intervalSec": 30}` | Changes telemetry publish interval |
| `FIRMWARE_UPDATE` | `{"url": "..."}` | Triggers OTA download (requires FOTA=y) |

## Application commands (this sample)

| Command | Payload | Effect |
|---------|---------|--------|
| `LED_ON` | `{}` | Turns on on-board LED (gpio0 pin 25) |
| `LED_OFF` | `{}` | Turns off on-board LED |
| `BUZZ` | `{"duration": 5}` | Custom command (extend for your hardware) |

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` | `60` | Telemetry every 60 seconds |
| `CONFIG_GPIO` | `y` | Required for LED_ON / LED_OFF |

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash
```

---

## How to test

1. Flash the firmware and wait for the device to connect
2. Open Conexio Console → your device → Commands tab
3. Select `LED_ON` from the dropdown and click **Send**
4. Serial log shows: `Command: LED_ON — LED is ON`
5. The on-board LED turns on
6. Send `LED_OFF` — LED turns off

For `SET_INTERVAL`:
- Payload: `{"intervalSec": 10}`
- The telemetry interval changes immediately

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Device Commands Sample ===
[00:00:00.257] <inf> app: LED ready (pin 25)
[00:00:19.533] <inf> mqtt_transport: MQTT connected to mqtt.cloud.conexiotech.com
[00:00:19.787] <inf> app: Connected — 355025934980275 | App v1.0.0 | SDK v2.3.0
...
[00:01:45.234] <inf> app: Command: LED_ON — LED is ON
[00:02:10.512] <inf> app: Command: LED_OFF — LED is OFF
```

---

## Command delivery guarantee

Commands use MQTT QoS 1. The broker retries delivery until the device
acknowledges. The SDK sends the ACK **before** dispatching to your handler,
so commands that cause a reboot (like `REBOOT`) won't create an infinite
reboot loop — the dashboard always sees `EXECUTED`.
