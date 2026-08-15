# Conexio Cloud SDK — Advanced Sample App

Production-grade firmware for Conexio Stratus / Stratus Pro (nRF91xx) built on
`conexio-cloud-sdk-v2` with nRF Connect SDK v3.2.1.

All infrastructure — PSM, offline buffering, FOTA, retry/watchdog, NTP sync,
certificate management, MQTT command delivery — is handled by the SDK.
The application only provides sensor callbacks, command handlers, and settings
handlers.

---

## What the application looks like

```c
#include <conexio_cloud/conexio_cloud.h>   /* one include — everything */

/* ── Named constants for all configurable limits ── */
#define TELEMETRY_INTERVAL_MIN_S   10      /* min: 10 s  (fast debug polling)     */
#define TELEMETRY_INTERVAL_MAX_S   7200    /* max: 7200 s (2 h battery sleep)     */
#define ALERT_THRESHOLD_MIN        0
#define ALERT_THRESHOLD_MAX        200

/* ── Sensor callbacks — SDK calls these before every publish ── */
static double read_temperature(void *arg) { return sensor_read_temp(); }
static double read_humidity(void *arg)    { return sensor_read_hum(); }

/* ── Settings callbacks — SDK validates range, then calls handler ── */
static enum conexio_setting_status on_alert_threshold(int32_t value, void *arg)
{
    g_alert_threshold = (int)value;   /* range already validated by SDK */
    LOG_INF("alertThreshold → %d", g_alert_threshold);
    return CONEXIO_SETTING_OK;
}

/* ── Command callbacks — hardware-specific only ── */
static void on_fan_on(const char *payload_json, void *arg)  { /* gpio on  */ }
static void on_fan_off(const char *payload_json, void *arg) { /* gpio off */ }

int main(void)
{
    /* 1. Sensors */
    conexio_cloud_register_sensor("temperature", read_temperature, NULL);
    conexio_cloud_register_sensor("humidity",    read_humidity,    NULL);

    /* 2. Commands  (REBOOT, SET_INTERVAL, FIRMWARE_UPDATE are built-in) */
    conexio_cloud_register_command("FAN_ON",  on_fan_on,  NULL);
    conexio_cloud_register_command("FAN_OFF", on_fan_off, NULL);

    /* 3. Settings with SDK-enforced range (telemetryIntervalSec is built-in) */
    conexio_cloud_register_setting_int_with_range("alertThreshold",
        ALERT_THRESHOLD_MIN, ALERT_THRESHOLD_MAX, on_alert_threshold, NULL);
    conexio_cloud_register_setting_bool("debugMode", on_debug_mode, NULL);

    /* 4. Interval limits — Golioth-style, two arguments */
    conexio_cloud_register_interval(TELEMETRY_INTERVAL_MIN_S,
                                    TELEMETRY_INTERVAL_MAX_S);

    /* 5. Single init call — handles everything */
    conexio_cloud_init(cloud_event_handler);

    /* 6. Main loop — SDK background thread does all the work */
    while (1) {
        k_sleep(K_SECONDS(conexio_cloud_get_interval_sec()));
    }
}
```

---

## Build and flash

```bash
# Build
west build -b conexio_stratus_pro

# Flash
west flash

# Monitor serial output (115200 baud)
screen /dev/tty.usbmodem* 115200
```

> **Prerequisites:** Flash `samples/conexio-stratus-provision` first so the
> device certificate and private key are stored in the modem security tags
> (100/101/102). This is a one-time step per device.

No endpoint configuration needed — the SDK fetches MQTT host, API key, and
Root CA from `config.conexio.io` at runtime using the device IMEI as identity.

---

## Sensor registration

### API

```c
int conexio_cloud_register_sensor(const char *name,
                                   conexio_sensor_read_cb_t callback,
                                   void *arg);
```

- `name` — metric key visible in the dashboard (e.g. `"temperature"`).
- `callback` — returns `double`. Return `NAN` to skip a cycle.
- `arg` — forwarded to the callback (device pointer, channel index, etc.).
- Must be called **before** `conexio_cloud_init()`.
- Maximum: `CONFIG_CONEXIO_CLOUD_MAX_SENSORS` (default 8).

### Common patterns

**Zephyr sensor driver**
```c
static const struct device *temp_dev = DEVICE_DT_GET(DT_ALIAS(temp_sensor));

static double read_temperature(void *arg) {
    const struct device *dev = (const struct device *)arg;
    struct sensor_value val;
    sensor_sample_fetch(dev);
    sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
    return sensor_value_to_double(&val);
}

conexio_cloud_register_sensor("temperature", read_temperature, (void *)temp_dev);
```

**Multiple sensors sharing one callback** (e.g. two ADC channels)
```c
static double read_soil(void *arg) {
    int ch = (int)(intptr_t)arg;
    return (adc_read_channel(ch) / 4095.0) * 100.0;  /* 0–100 % */
}

conexio_cloud_register_sensor("soil_a", read_soil, (void *)(intptr_t)0);
conexio_cloud_register_sensor("soil_b", read_soil, (void *)(intptr_t)1);
```

**Skip a cycle when sensor is unavailable**
```c
static double read_wind(void *arg) {
    if (!anemometer_data_ready()) return NAN;   /* omit this cycle */
    return anemometer_get_speed_ms();
}
```

**GPS coordinates (two callbacks)**
```c
conexio_cloud_register_sensor("gps_lat", read_gps_lat, NULL);
conexio_cloud_register_sensor("gps_lon", read_gps_lon, NULL);
```

**Digital state as numeric**
```c
static double read_door(void *arg) {
    return gpio_pin_get(door_dev, DOOR_PIN) ? 1.0 : 0.0;
}
conexio_cloud_register_sensor("door_open", read_door, NULL);
```

### Event-driven metrics (not sensor callbacks)

For non-periodic or non-numeric data, queue metrics directly:

```c
conexio_cloud_send_metric("impulse_count", (double)pulse_counter);
conexio_cloud_send_metric_str("last_error", "NRF_ENOBUFS");
conexio_cloud_send_metric_bool("overheat_alarm", true);
```

---

## Commands

Commands arrive from the **Device Commands** page on the Conexio Console and
are delivered over MQTT (QoS 1, persistent session). The SDK guarantees
at-least-once delivery — the MQTT PUBACK is sent **before** the command handler
runs, preventing infinite reboot loops when the handler calls `sys_reboot()`.

### Built-in commands (no registration needed)

| Command | Payload | Effect |
|---|---|---|
| `REBOOT` | `{}` | `sys_reboot(SYS_REBOOT_COLD)` after 500 ms flush |
| `SET_INTERVAL` | `{"interval": 120}` | Updates publish interval in seconds |
| `FIRMWARE_UPDATE` | `{"jobId":"…","document":{…}}` | Starts FOTA download |

### Telemetry interval — Golioth-style registration

Declare valid limits as named `#define` constants. The SDK rejects
out-of-range values with a log warning; the handler is never called.

```c
#define TELEMETRY_INTERVAL_MIN_S   10      /* minimum: 10 seconds */
#define TELEMETRY_INTERVAL_MAX_S   7200    /* maximum: 2 hours    */

conexio_cloud_register_interval(TELEMETRY_INTERVAL_MIN_S,
                                TELEMETRY_INTERVAL_MAX_S);
```

- Valid range: `10–7200` seconds. Values outside this are silently rejected.
- After a reboot the interval resets to `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC`
  (default `20` in `prj.conf`) until a new `SET_INTERVAL` arrives.
- `conexio_cloud_get_interval_sec()` always returns the current live value.

### Custom hardware commands

Register handlers for your own commands. The SDK passes the raw JSON payload
string — parse with `cJSON` if needed.

```c
static void on_fan_on(const char *payload_json, void *arg)
{
    /* Payload example from dashboard: {"speed": 80} */
    int speed = 100;
    if (payload_json) {
        cJSON *p = cJSON_Parse(payload_json);
        const cJSON *s = cJSON_GetObjectItem(p, "speed");
        if (cJSON_IsNumber(s)) speed = (int)s->valuedouble;
        cJSON_Delete(p);
    }
    fan_set_speed(speed);
    LOG_INF("FAN_ON at %d%%", speed);
}

conexio_cloud_register_command("FAN_ON", on_fan_on, NULL);
```

Maximum commands: `CONFIG_CONEXIO_CLOUD_MAX_COMMANDS` (default 16).

---

## Settings (OTA Config)

Settings arrive from the **OTA Config** page on the Conexio Console. The
dashboard pushes a JSON object; the SDK dispatches each key to its registered
handler. Settings persist across publishes but reset on reboot (use NVS to
persist if needed).

### Plain registration (app validates range)

```c
static enum conexio_setting_status on_threshold(int32_t value, void *arg)
{
    if (value < 0 || value > 200) return CONEXIO_SETTING_VALUE_OUT_OF_RANGE;
    g_threshold = (int)value;
    return CONEXIO_SETTING_OK;
}

conexio_cloud_register_setting_int("alertThreshold", on_threshold, NULL);
```

### With range — Golioth-style (SDK validates, callback is clean)

```c
#define ALERT_THRESHOLD_MIN   0
#define ALERT_THRESHOLD_MAX   200

static enum conexio_setting_status on_threshold(int32_t value, void *arg)
{
    g_threshold = (int)value;   /* SDK already validated — no if() needed */
    LOG_INF("alertThreshold → %d", g_threshold);
    return CONEXIO_SETTING_OK;
}

conexio_cloud_register_setting_int_with_range("alertThreshold",
    ALERT_THRESHOLD_MIN, ALERT_THRESHOLD_MAX, on_threshold, NULL);
```

When an out-of-range value is pushed from the dashboard the SDK logs:
```
Setting 'alertThreshold': 250 out of range [0, 200] — rejected
```
The handler is never called and the current value is unchanged.

### All setting types

| Function | Dashboard sends | C type in callback |
|---|---|---|
| `conexio_cloud_register_setting_int(key, cb, arg)` | `{"key": 42}` | `int32_t` |
| `conexio_cloud_register_setting_int_with_range(key, min, max, cb, arg)` | `{"key": 42}` | `int32_t` — pre-validated |
| `conexio_cloud_register_setting_bool(key, cb, arg)` | `{"key": true}` | `bool` |
| `conexio_cloud_register_setting_float(key, cb, arg)` | `{"key": 3.14}` | `float` |
| `conexio_cloud_register_setting_float_with_range(key, min, max, cb, arg)` | `{"key": 3.14}` | `float` — pre-validated |
| `conexio_cloud_register_setting_string(key, cb, arg)` | `{"key": "hello"}` | `const char *` + `size_t` |

### Return codes from setting handlers

| Code | Meaning |
|---|---|
| `CONEXIO_SETTING_OK` | Accepted and applied |
| `CONEXIO_SETTING_VALUE_OUT_OF_RANGE` | Value rejected (logged as warning) |
| `CONEXIO_SETTING_VALUE_WRONG_TYPE` | Type mismatch (SDK catches this first) |
| `CONEXIO_SETTING_ERROR` | General failure |

---

## Automatic metrics

Included in every telemetry payload with no application code.

| Metric | Description | Frequency |
|---|---|---|
| `_rssi` | LTE RSRP signal strength (dBm) | Every publish |
| `_snr` | Signal-to-noise ratio | Every publish |
| `_reboot_cnt` | NVS-persisted boot counter | Boot-once |
| `_reboot_reason` | Reset cause (`software`, `watchdog`, `por`, …) | Boot-once |
| `_sdk_version` | SDK semantic version string | Boot-once |
| `_modem_fw` | Modem firmware version | Boot-once |
| `_operator` | Network operator name | Boot-once |
| `_lte_mode` | Radio mode: 7=LTE-M, 9=NB-IoT | Boot-once |
| `_lte_band` | Active LTE band number | Every 10 publishes |
| `_cell_id` | E-UTRAN cell ID | Every 10 publishes |
| `_tac` | Tracking Area Code | Every 10 publishes |
| `_lte_connect_ms` | Time to first LTE registration (ms) | Boot-once |
| `_conn_loss` | LTE drop+re-register count since boot | Every publish |
| `_psm_tau_sec` | Granted PSM TAU period (s) | Boot-once |
| `_psm_active_sec` | Granted PSM active window (s) | Boot-once |
| `_edrx_ms` | Granted eDRX interval (ms) | Boot-once |
| `_modem_temp` | Modem die temperature (°C) | Every publish |
| `_battery_mv` | Battery voltage via AT%XVBAT (mV) | Every publish (opt-in) |
| `_tx_kb` | Kilobytes transmitted this session | Every publish |
| `_rx_kb` | Kilobytes received this session | Every publish |

Enable battery voltage reporting:
```ini
CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y
```

---

## Production features (prj.conf)

```ini
# ── Core SDK (20 s default interval) ─────────────────────────────────────────
CONFIG_CONEXIO_CLOUD=y
CONFIG_CONEXIO_CLOUD_MQTT=y
CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=20

# ── LTE Power Saving Mode ─────────────────────────────────────────────────────
CONFIG_CONEXIO_CLOUD_PSM=y
CONFIG_CONEXIO_CLOUD_PSM_TAU_SEC=7200        # TAU keepalive every 2 hours
CONFIG_CONEXIO_CLOUD_PSM_ACTIVE_TIME_SEC=30  # 30 s radio-on window after TX

# ── Offline telemetry buffer (NVS flash ring buffer) ──────────────────────────
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE=100
CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH=10

# ── FOTA via AWS IoT Jobs + MCUboot ───────────────────────────────────────────
CONFIG_CONEXIO_CLOUD_FOTA=y
CONFIG_CONEXIO_CLOUD_FOTA_DOWNLOAD_TIMEOUT_SEC=300

# ── Exponential backoff retry + hardware watchdog ─────────────────────────────
CONFIG_CONEXIO_CLOUD_RETRY=y
CONFIG_CONEXIO_CLOUD_RETRY_BASE_SEC=5
CONFIG_CONEXIO_CLOUD_RETRY_MAX_SEC=300
CONFIG_CONEXIO_CLOUD_RETRY_MAX_ATTEMPTS=10
CONFIG_CONEXIO_CLOUD_WATCHDOG_TIMEOUT_SEC=600

# ── Battery voltage in every payload ──────────────────────────────────────────
CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y
```

---

## Testing commands from the dashboard

**Prerequisites:** device online (green dot in All Devices list).

### REBOOT

1. Dashboard → Device Commands → select device → click **REBOOT** preset
2. Click **Send Command**
3. Serial output within 1–2 seconds:
   ```
   MQTT message received on topic: devices/<imei>/commands (N bytes)
   REBOOT command — rebooting in 500 ms
   === Conexio Advanced Sample (SDK 2.1.0) ===
   ```
4. Device reboots **once**. The MQTT PUBACK is sent before `sys_reboot()` so
   AWS IoT Core removes the message — no repeat reboot on reconnect.

### SET_INTERVAL

1. Dashboard → Device Commands → click **SET_INTERVAL** preset
2. Edit payload to your desired interval: `{"interval": 60}`
   - Valid range: `10–7200` seconds (enforced by firmware)
   - After reboot: resets to `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` (20 s)
3. Click **Send Command**
4. Serial output:
   ```
   SET_INTERVAL: publish interval → 60s
   ```

### SET_THRESHOLD (OTA Config)

Use the **OTA Config** page (not Commands) to push settings:
```json
{ "alertThreshold": 95 }
```
Serial output:
```
Setting 'alertThreshold' applied successfully
Setting: alertThreshold → 95
```

Out-of-range example (`{ "alertThreshold": 250 }`):
```
Setting 'alertThreshold': 250 out of range [0, 200] — rejected
```

### Simulating a device (no hardware)

Use the command listener script to simulate a device polling and acknowledging
commands directly from DynamoDB:

```bash
python3 scripts/command_listener.py --device <imei> --interval 2
```

---

## SDK module overview

| Module | Kconfig | Description |
|---|---|---|
| `conexio_cloud.c` | `CONFIG_CONEXIO_CLOUD` | Core: command/setting dispatch, payload builder, background thread |
| `transport/mqtt_transport.c` | `CONFIG_CONEXIO_CLOUD_MQTT` | MQTT over TLS, QoS 1, persistent session, command drain on reconnect |
| `power_mgr.c` | `CONFIG_CONEXIO_CLOUD_PSM` | PSM/eDRX negotiation, modem sleep/wake |
| `offline_buffer.c` | `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER` | NVS ring buffer, replay on reconnect |
| `fota.c` | `CONFIG_CONEXIO_CLOUD_FOTA` | AWS IoT Jobs FOTA, MCUboot image confirm |
| `retry.c` | `CONFIG_CONEXIO_CLOUD_RETRY` | Exponential backoff, hardware watchdog |
| `cert_store.c` | always | TLS credential provisioning (Root CA download) |
| `lte.c` | `CONFIG_CONEXIO_CLOUD_MANAGE_LTE` | LTE connect, session metrics |
| `config_fetch.c` | always | Runtime config fetch from `config.conexio.io` |

---

## FOTA job document format

The Conexio Console Firmware page creates AWS IoT Jobs with this document:

```json
{
  "operation": "firmware_update",
  "firmwareVersion": "1.4.2",
  "location": {
    "url": "https://iot-dashboard-firmware.s3.amazonaws.com/..."
  },
  "checksum": "sha256:abc123...",
  "fileSize": 131072
}
```

The URL is a 24-hour pre-signed S3 GET — no AWS credentials needed on the device.
After download MCUboot verifies the checksum. On the new firmware's first boot
`fota_confirm()` marks the image valid; MCUboot reverts if confirm is never called.
