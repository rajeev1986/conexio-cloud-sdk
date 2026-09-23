# Device Settings — OTA Config JSON Settings

Demonstrates receiving JSON configuration settings from the Conexio Console
and applying them at runtime — no firmware reflash required.

Settings (OTA Config) are pushed from the Console to the device over MQTT
(QoS 1). The SDK calls your handler when a setting arrives. Changes take
effect immediately.

---

## What this sample does

- Registers two application settings:
  - `alertThreshold` (integer, 0–100) — temperature alert trigger in °C
  - `reportingMode` (string) — `"normal"` | `"verbose"` | `"silent"`
- Built-in SDK setting `telemetryIntervalSec` works automatically
- Logs every received setting update

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` | `60` | Default telemetry interval |
| `CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING` | `y` | Built-in telemetryIntervalSec handling |

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
newtmgr -c serial image upload build/device_settings/zephyr/zephyr.signed.bin
```

---

## How to test

1. Flash and wait for the device to connect
2. Open Conexio Console → your device → OTA Config tab
3. Add a setting:
   - Key: `alertThreshold` | Type: Integer | Value: `28`
4. Click **Push** — the device receives it at the next MQTT poll
5. Serial shows: `Setting: alertThreshold → 28 °C`
6. Try `telemetryIntervalSec` with value `10` — watch the publish rate change

---

## Setting types reference

```c
// Integer — with optional bounds validation
conexio_cloud_register_setting_int_with_range("key", min, max, handler, NULL);

// Integer — no bounds check
conexio_cloud_register_setting_int("key", handler, NULL);

// Boolean
conexio_cloud_register_setting_bool("key", handler, NULL);

// String
conexio_cloud_register_setting_str("key", handler, NULL);
```

Return `CONEXIO_SETTING_OK` to accept or `CONEXIO_SETTING_REJECTED` to refuse.

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Device Settings Sample ===
[00:00:00.257] <inf> app: Defaults: threshold=35°C logging=on mode=normal
[00:00:19.533] <inf> mqtt_transport: MQTT connected
[00:00:19.787] <inf> app: Connected — 355025934980275 | threshold=35°C mode=normal
...
[00:01:05.234] <inf> app: Setting: alertThreshold → 28 °C
[00:01:10.512] <inf> app: Setting: reportingMode → verbose
```

---

## Cloud settings payload (pushed by Console)

```json
{
  "type": "config",
  "settings": {
    "alertThreshold": 28,
    "reportingMode": "verbose"
  }
}
```
