# SHT4x Real Sensor Integration

This sample now publishes **both** simulated sensor data (existing) and real SHT4x
temperature and humidity readings via the QWIIC port.

## New metrics published

| Metric name | Source | Unit |
|---|---|---|
| `temperature` | Simulated (random 20–40 °C) | °C |
| `humidity` | Simulated (random 40–80 %) | % |
| `temperature_real` | SHT4x hardware sensor | °C |
| `humidity_real` | SHT4x hardware sensor | % |

`temperature_real` and `humidity_real` are omitted from the CBOR payload if the
SHT4x is not present or not ready (the SDK silently skips NaN-valued metrics).

## Files changed

### `boards/conexio_stratus_pro_nrf9151_ns.overlay`

Added the `&i2c2` node with the SHT4x child at address `0x44` on I2C_BITRATE_FAST.
The existing partition `#include` lines were preserved.

```dts
&i2c2 {
    status = "okay";
    clock-frequency = <I2C_BITRATE_FAST>;
    sht4x@44 {
        status = "okay";
        compatible = "sensirion,sht4x";
        reg = <0x44>;
        repeatability = <2>;
    };
};
```

### `prj.conf`

Appended three Kconfig entries (all existing lines preserved):

```kconfig
CONFIG_SENSOR=y
CONFIG_I2C=y
CONFIG_SHT4X=y
```

`CONFIG_SHT4X` auto-selects `CONFIG_I2C` and `CONFIG_CRC` when the DT node is
`status = "okay"`, but the entries are added explicitly for documentation clarity.

### `src/main.c`

Three additions:

1. **Static device pointer** `s_sht4x` guarded by
   `#if DT_HAS_COMPAT_STATUS_OKAY(sensirion_sht4x)`.

2. **Real sensor callbacks** `read_temperature_real()` and `read_humidity_real()`:
   - `read_temperature_real` calls `sensor_sample_fetch()` then
     `sensor_channel_get(SENSOR_CHAN_AMBIENT_TEMP)`.
   - `read_humidity_real` calls `sensor_channel_get(SENSOR_CHAN_HUMIDITY)` only —
     no second I2C transaction, reads from the driver's internal buffer.
   - Both return `NAN` when `s_sht4x == NULL` or `!device_is_ready(s_sht4x)`.
   - On boards without the DT node, `#else` stubs return `NAN` unconditionally.

3. **Registrations in `main()`**:
   ```c
   conexio_cloud_register_sensor("temperature_real", read_temperature_real, NULL);
   conexio_cloud_register_sensor("humidity_real",    read_humidity_real,    NULL);
   ```
   Registered immediately after the existing `temperature`/`humidity` registrations.
   `temperature_real` is always registered before `humidity_real` so the fetch
   always precedes the channel-only read.

## Build

The sample builds with zero errors and zero warnings for target
`conexio_stratus_pro/nrf9151/ns`. The SHT4x driver library is confirmed compiled in:

```
[381/409] Linking C static library
  zephyr/drivers/sensor/sensirion/sht4x/libdrivers__sensor__sensirion__sht4x.a
```

See `.agents/sht4x-integration/build-evidence.md` for the full build log.

## Runtime behaviour

- **Sensor present and ready**: Both `temperature_real` and `humidity_real` are
  included in every publish with actual measured values.
- **Sensor absent or I2C error**: Callbacks return `NaN`; the SDK omits those
  keys from the payload. Simulated `temperature`/`humidity` continue publishing
  normally.
- Serial log on boot shows `SHT4x ready — real sensor metrics enabled` or
  `SHT4x not ready — real sensor metrics will be skipped`.

## What was NOT changed

- Simulated `read_temperature()` / `read_humidity()` and their
  `CONFIG_CONEXIO_SAMPLE_SIMULATED_SENSORS` guard — untouched.
- `conexio_cloud_register_sensor("temperature", ...)` and `"humidity"` — untouched.
- `alertThreshold` setting registration — untouched.
- `CMakeLists.txt`, `Kconfig.sysbuild` — untouched.
- All pre-existing `prj.conf` entries — preserved; new entries only appended.
- Overlay partition `#include` lines — preserved; `&i2c2` block appended after.
