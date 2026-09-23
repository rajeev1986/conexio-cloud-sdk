# Sensor Data — Streaming Sensor Readings

Demonstrates fetching sensor readings (temperature, humidity) and
streaming them to Conexio Cloud on a configurable interval.

By default the sample uses **simulated sensors** that return random values.
Replace `read_temperature()` / `read_humidity()` with real Zephyr sensor
driver calls for production hardware.

---

## What this sample does

1. Registers two sensor callbacks: `temperature` (°C) and `humidity` (%)
2. Registers an `alertThreshold` setting — updateable from the Console at runtime
3. Publishes telemetry every `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` seconds
4. Fires an alert to `v1/devices/{id}/alerts` when temperature exceeds threshold

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` | `30` | Publish every 30 seconds |
| `CONFIG_CONEXIO_CLOUD_CBOR` | `y` | CBOR binary encoding |
| `CONFIG_CONEXIO_CLOUD_PSM` | `y` | Modem sleeps between publishes |
| `CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS` | `y` | Random values (set `n` for real hardware) |

---

## Build and flash

```bash
# Default — simulated sensors
west build -b conexio_stratus_pro/nrf9151/ns --pristine
newtmgr -c serial image upload build/sensor_data/zephyr/zephyr.signed.bin

# Real sensors (implement read_temperature/read_humidity in main.c first)
west build -b conexio_stratus_pro/nrf9151/ns --pristine -- \
  -DCONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS=n
```

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Sensor Data Sample ===
[00:00:00.257] <inf> app: Interval: 30s | Simulated: yes
[00:00:19.533] <inf> mqtt_transport: MQTT connected to mqtt.cloud.conexiotech.com
[00:00:19.787] <inf> app: Connected — 355025934980275 | App v1.0.0
[00:00:21.900] <inf> app: Telemetry published
```

---

## Cloud JSON payload

```json
{
  "dev_id": "355025934980275",
  "ts": "2026-09-20T14:00:00.000Z",
  "metrics": {
    "temperature": 27.3,
    "humidity": 63.5,
    "_rssi": 57,
    "_snr": 9,
    "_reboot_cnt": 1,
    "_sdk": "2.3.0",
    "_fw_ver": "1.0.0"
  }
}
```

### Alert payload (when temperature ≥ alertThreshold)

Sent immediately to `v1/devices/{id}/alerts` — does not wait for the next publish interval:

```json
{
  "dev_id": "355025934980275",
  "ts": "2026-09-20T14:00:15.000Z",
  "metric": "temperature",
  "value": 36.1,
  "threshold": 35
}
```

---

## Replacing simulated sensors

```c
/* Example: BME280 temperature sensor */
static double read_temperature(void *arg)
{
    struct sensor_value val;
    sensor_sample_fetch(bme280_dev);
    sensor_channel_get(bme280_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
    return sensor_value_to_double(&val);
}
```
