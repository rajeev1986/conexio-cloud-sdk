# Conexio Cloud SDK — Samples

Standalone sample applications for the Conexio Stratus Pro (nRF9151).
Each sample demonstrates one specific feature of the Conexio Cloud SDK.

---

## Sample Index

| Sample | Feature | Complexity |
|--------|---------|------------|
| [`hello/`](hello/) | Minimal cloud connection + single metric | ⭐ Beginner |
| [`sensor_data/`](sensor_data/) | Streaming temperature & humidity | ⭐ Beginner |
| [`device_commands/`](device_commands/) | Handling cloud commands (LED, REBOOT) | ⭐⭐ Intermediate |
| [`device_settings/`](device_settings/) | OTA Config JSON settings from Console | ⭐⭐ Intermediate |
| [`device_schedules/`](device_schedules/) | Timed commands with NVS watchdog | ⭐⭐ Intermediate |
| [`cellular_location/`](cellular_location/) | AT%NCELLMEAS cell-based positioning | ⭐⭐ Intermediate |
| [`offline_buffer/`](offline_buffer/) | Flash-backed buffering during outages | ⭐⭐ Intermediate |
| [`fota/`](fota/) | Over-the-air firmware updates | ⭐⭐⭐ Advanced |
| [`app/`](app/) | All features combined — production template | ⭐⭐⭐ Advanced |

---

## Recommended learning path

```
hello → sensor_data → device_commands → device_settings
      ↓
device_schedules → cellular_location → offline_buffer → fota
      ↓
     app  (combine everything into your own application)
```

---

## Build all samples

```bash
# Build any sample (replace <sample> with the folder name)
cd samples/<sample>
west build -b conexio_stratus_pro/nrf9151/ns --pristine
newtmgr -c serial image upload build/<sample>/zephyr/zephyr.signed.bin
```

---

## Common prj.conf options

All samples share these base settings:

```kconfig
CONFIG_CONEXIO_CLOUD=y              # Enable the SDK
CONFIG_CONEXIO_CLOUD_CBOR=y         # Binary encoding (50–70% smaller than JSON)
CONFIG_CONEXIO_CLOUD_RETRY=y        # Exponential backoff reconnect
CONFIG_NEWLIB_LIBC=y                # C library (required for TLS sockets)
CONFIG_POSIX_THREADS=y              # Threading (required by date_time module)
```

---

## Ingest key quick-connect (no fleet provisioning)

All samples support the ingest key authentication path. Create an overlay:

```kconfig
# prj_my_key.conf — never commit real keys to git
CONFIG_CONEXIO_CLOUD_INGEST_KEY="cnx_xxxxxxxx_..."
```

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine -- \
  -DEXTRA_CONF_FILE=prj_my_key.conf
```

The device IMEI must be pre-registered in Conexio Console → All Devices.
