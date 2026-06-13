# Conexio Cloud SDK — Advanced Sample App

Production-grade firmware for nRF91xx devices built on `conexio-cloud-sdk-v2`.

All infrastructure (PSM, offline buffering, FOTA, retry/watchdog, NTP sync,
certificate management) is handled by the SDK. The application only provides
sensor callbacks, actuator command handlers, and settings handlers.

---

## What the application looks like

```c
#include <conexio_cloud/conexio_cloud.h>  /* one include — everything */

// Sensor callbacks: SDK calls these before every publish
static double read_temperature(void *arg) { return sensor_read_temp(); }
static double read_humidity(void *arg)    { return sensor_read_hum(); }

void main(void) {
    // Register sensors — SDK reads them automatically before each publish
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);

    // Register application commands (REBOOT, SET_INTERVAL, FIRMWARE_UPDATE
    // are built-in — only register your own hardware commands here)
    conexio_cloud_register_command("FAN_ON",  on_fan_on,  NULL);
    conexio_cloud_register_command("FAN_OFF", on_fan_off, NULL);

    // Register application settings (telemetryIntervalSec is built-in)
    conexio_cloud_register_setting_int( "alertThreshold", on_threshold, NULL);
    conexio_cloud_register_setting_bool("debugMode",      on_debug,     NULL);

    // One init call — handles everything
    conexio_cloud_init(cloud_event_handler);

    // Main loop — the SDK does all the work
    while (1) {
        k_sleep(K_SECONDS(conexio_cloud_get_interval_sec()));
    }
}
```

---

## Sensor registration API

`conexio_cloud_register_sensor()` is the primary way to publish sensor data.
Register callbacks before `conexio_cloud_init()`. The SDK background thread calls
each registered callback before every telemetry publish.

### Callback signature

```c
typedef double (*conexio_sensor_read_cb_t)(void *arg);
```

- Returns `double` — covers all numeric sensor types.
- Return `NAN` to skip a reading for the current cycle (sensor not ready,
  warming up, or temporarily unavailable). The SDK omits that metric from the
  payload for that cycle; no gap appears in the dashboard.
- `arg` is forwarded from registration — use it to pass a device pointer,
  channel index, or any other context without needing globals.

### Registration

```c
int conexio_cloud_register_sensor(const char *name,
                                   conexio_sensor_read_cb_t callback,
                                   void *arg);
```

- `name` — the metric key as it appears in the dashboard. Any string.
- Must be called before `conexio_cloud_init()`.
- Maximum sensors: `CONFIG_CONEXIO_CLOUD_MAX_SENSORS` (default 8).

### Examples — common sensor types

**Zephyr sensor driver (generic)**
```c
// Get the device handle from the device tree at compile time.
// The DT node alias must match your board's device tree (e.g. boards/nrf9160dk.overlay).
// Example: &bme280 { compatible = "bosch,bme280"; ... };
#include <zephyr/drivers/sensor.h>

static const struct device *temp_dev = DEVICE_DT_GET(DT_ALIAS(temp_sensor));

// Callback — arg is the device pointer passed at registration
static double read_temperature(void *arg) {
    const struct device *dev = (const struct device *)arg;
    struct sensor_value val;
    sensor_sample_fetch(dev);
    sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
    return sensor_value_to_double(&val);
}

// In main(), before conexio_cloud_init():
// Verify the device is ready before registering
if (!device_is_ready(temp_dev)) {
    LOG_ERR("Temperature sensor not ready");
} else {
    conexio_cloud_register_sensor("temperature", read_temperature, (void *)temp_dev);
}
```

**Multiple sensors, same callback**
```c
// Two soil probes on the same ADC, different channels.
// Cast the channel index to void* so no extra struct is needed.
static double read_soil(void *arg) {
    int channel = (int)(intptr_t)arg;
    int raw = adc_read_channel(channel);   // your ADC read function
    return (raw / 4095.0) * 100.0;        // 0–100 %
}

conexio_cloud_register_sensor("soil_zone_a", read_soil, (void *)(intptr_t)0);
conexio_cloud_register_sensor("soil_zone_b", read_soil, (void *)(intptr_t)1);
```

**Pressure sensor (BMP280)**
```c
// Device handle from DT — same pattern as temperature above
static const struct device *bmp_dev = DEVICE_DT_GET(DT_ALIAS(pressure_sensor));

static double read_pressure(void *arg) {
    const struct device *dev = (const struct device *)arg;
    struct sensor_value val;
    sensor_sample_fetch(dev);
    sensor_channel_get(dev, SENSOR_CHAN_PRESS, &val);
    return sensor_value_to_double(&val);   // hPa
}

conexio_cloud_register_sensor("pressure", read_pressure, (void *)bmp_dev);
```

**CO₂ sensor (UART-based, e.g. SCD30)**
```c
static double read_co2(void *arg) {
    ARG_UNUSED(arg);
    return (double)scd30_get_co2_ppm();   // ppm
}

conexio_cloud_register_sensor("co2", read_co2, NULL);
```

**Battery voltage (if not using `CONFIG_CONEXIO_CLOUD_AUTO_BATTERY`)**
```c
static double read_battery(void *arg) {
    ARG_UNUSED(arg);
    return (double)adc_read_battery_mv();  // mV
}

conexio_cloud_register_sensor("battery_mv", read_battery, NULL);
```

**Sensor with NAN skip (not ready on first cycles)**
```c
static double read_wind_speed(void *arg) {
    ARG_UNUSED(arg);
    if (!anemometer_data_ready()) return NAN;   // omit this cycle
    return anemometer_get_speed_ms();           // m/s
}

conexio_cloud_register_sensor("wind_speed_ms", read_wind_speed, NULL);
```

**GPS latitude/longitude (two callbacks, one per axis)**
```c
static double read_gps_lat(void *arg)  { ARG_UNUSED(arg); return gps_get_lat(); }
static double read_gps_lon(void *arg)  { ARG_UNUSED(arg); return gps_get_lon(); }
static double read_gps_alt(void *arg)  { ARG_UNUSED(arg); return gps_get_alt_m(); }

conexio_cloud_register_sensor("gps_lat", read_gps_lat, NULL);
conexio_cloud_register_sensor("gps_lon", read_gps_lon, NULL);
conexio_cloud_register_sensor("gps_alt", read_gps_alt, NULL);
```

**Digital state as numeric (door, relay, valve)**
```c
static double read_door(void *arg) {
    ARG_UNUSED(arg);
    return gpio_pin_get(door_dev, DOOR_PIN) ? 1.0 : 0.0;
}

conexio_cloud_register_sensor("door_open", read_door, NULL);
```

### When to use `send_metric()` instead

`conexio_cloud_register_sensor()` is designed for **periodic numeric readings**.
For event-driven or non-numeric data, use the queue API directly:

```c
// String metric — GPS NMEA sentence, firmware version, error code
conexio_cloud_send_metric_str("last_error", "NRF_ENOBUFS");

// Boolean metric — alarm state
conexio_cloud_send_metric_bool("overheat_alarm", true);

// Event-driven numeric — call from an interrupt or timer callback
conexio_cloud_send_metric("impulse_count", (double)pulse_counter);
```

---

## Built-in SDK features (zero application code)

### Auto-registered commands

| Command | Payload | What the SDK does |
|---|---|---|
| `REBOOT` | none | `sys_reboot(SYS_REBOOT_COLD)` after 500 ms log flush |
| `SET_INTERVAL` | `{"interval": 120}` | Updates publish interval at runtime (seconds) |
| `FIRMWARE_UPDATE` | `{"jobId":"...", "document":{...}}` | Starts FOTA download (if `CONFIG_CONEXIO_CLOUD_FOTA=y`) |

### Auto-registered setting

| Key | Type | Effect |
|---|---|---|
| `telemetryIntervalSec` | int | Updates SDK publish interval (same as SET_INTERVAL) |

Disable with `CONFIG_CONEXIO_CLOUD_AUTO_INTERVAL_SETTING=n` if you need custom validation.

### Auto-metrics in every payload

| Metric | Source | Dashboard page |
|---|---|---|
| `_rssi` | Modem RSRP (dBm) | Fleet Health → Device Signal |
| `_snr` | Modem SNR | Fleet Health → Device Signal |
| `_reboot_cnt` | NVS-persisted boot counter | Fleet Health → Reboot Tracking |
| `_sdk_version` | SDK version string | Diagnostic — visible in any chart |
| `_battery_mv` | Modem VDDMAIN via AT%XVBAT | Any chart (opt-in: `CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y`) |

### Decommissioned device handling

If the Conexio Console decommissions the device, the HTTP ingestor returns `403`.
The SDK detects this, logs an actionable error message, powers off the modem radio,
and sleeps indefinitely — stopping the device from hammering the server.
Re-provision via the dashboard to recover.

---

## Enabling production features

All features are opt-in via `prj.conf`. No code changes required.

```ini
# ── Core SDK (Phase 2 — zero endpoint configuration) ─────────────────────────
CONFIG_CONEXIO_CLOUD=y
CONFIG_CONEXIO_CLOUD_MQTT=y
CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=60

# ── LTE Power Saving Mode (~2–3 µA sleep between publishes) ──────────────────
CONFIG_CONEXIO_CLOUD_PSM=y
CONFIG_CONEXIO_CLOUD_PSM_TAU_SEC=3600        # network keepalive every hour
CONFIG_CONEXIO_CLOUD_PSM_ACTIVE_TIME_SEC=10  # 10s window to TX and sleep

# ── Offline telemetry buffer (NVS flash ring buffer) ──────────────────────────
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE=100   # ~100 min at 60s interval
CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH=10   # replay 10 per reconnect

# ── FOTA via AWS IoT Jobs + MCUboot ───────────────────────────────────────────
CONFIG_CONEXIO_CLOUD_FOTA=y
CONFIG_CONEXIO_CLOUD_FOTA_DOWNLOAD_TIMEOUT_SEC=300

# ── Exponential backoff retry + hardware watchdog ─────────────────────────────
CONFIG_CONEXIO_CLOUD_RETRY=y
CONFIG_CONEXIO_CLOUD_RETRY_BASE_SEC=5
CONFIG_CONEXIO_CLOUD_RETRY_MAX_SEC=300
CONFIG_CONEXIO_CLOUD_RETRY_MAX_ATTEMPTS=10
CONFIG_CONEXIO_CLOUD_WATCHDOG_TIMEOUT_SEC=600

# ── Optional: auto-include battery voltage in every payload ───────────────────
# CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y
```

---

## Build

```bash
west build -b nrf9160dk/nrf9160/ns
west flash
```

No endpoint configuration needed — the SDK fetches MQTT host, API key, and
Root CA from `config.conexio.io` at runtime using the device IMEI.

---

## FOTA job document format

The Conexio Console Firmware page creates AWS IoT Jobs with this document:

```json
{
  "operation": "firmware_update",
  "firmwareVersion": "1.4.2",
  "location": {
    "url": "https://iot-dashboard-firmware-<account>.s3.amazonaws.com/..."
  },
  "checksum": "sha256:abc123...",
  "fileSize": 131072
}
```

Parse `location.url` (not `firmware.url`) for the download URL. The URL is a
24-hour pre-signed S3 GET URL — no AWS credentials needed on the device.

---

## SDK module overview

These modules live in `conexio-cloud-sdk-v2/src/` and are compiled in based
on Kconfig. Applications include them via the umbrella header only.

### `power_mgr.c` — LTE PSM/eDRX
Negotiates PSM with the LTE network via `lte_lc_psm_req()`. After each publish
the modem enters deep sleep (T3412 timer) at ~2–3 µA. Wakes on TAU expiry or
network page. Reduces battery consumption by 5–20× vs always-on.

### `offline_buffer.c` — NVS ring buffer
Stores telemetry JSON payloads in flash via Zephyr NVS when the device is
offline. On reconnect the SDK replays buffered payloads oldest-first before
resuming live telemetry. Ring buffer: oldest entries evicted when full.
Skips payloads with epoch timestamps (NTP not yet synced) to prevent
corrupting dashboard time-series charts.

### `fota.c` — OTA firmware updates
Triggered by a `FIRMWARE_UPDATE` command from the Firmware page. Downloads
the binary from the pre-signed S3 URL via `fota_download`, writes it to the
secondary MCUboot partition, and reboots. On the new firmware's first boot
`fota_confirm()` marks the image valid; MCUboot reverts to the previous
version if confirm is never called.

### `retry.c` — Exponential backoff + hardware watchdog
On each connection failure: `delay = min(base × 2ⁿ + jitter, max)`.
After `max_attempts` consecutive failures: `sys_reboot(SYS_REBOOT_COLD)`.
Hardware watchdog is armed in `retry_init()` and fed by the SDK background
thread on every loop iteration. Catches modem hangs, deadlocks, and
infinite retry loops — device reboots automatically.
