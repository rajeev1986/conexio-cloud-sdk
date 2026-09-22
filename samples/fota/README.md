# FOTA — Firmware Over-the-Air Updates

Demonstrates setting up and handling over-the-air firmware updates delivered
via the Conexio Console Firmware page.

---

## What this sample does

- Enables FOTA via `CONFIG_CONEXIO_CLOUD_FOTA=y`
- Registers an optional FOTA event callback to log download progress
- Shows how to register a maintenance window callback to defer updates

---

## How FOTA works end-to-end

```
1. You upload firmware binary    → Conexio Console → Firmware
2. Console creates a job         → targets this device (or all devices)
3. Device receives FIRMWARE_UPDATE command with signed download URL
4. SDK downloads binary over HTTPS (nRF9151 modem HTTPS client)
5. MCUboot applies update on next reboot
6. _fw_ver in next telemetry confirms new version
```

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_FOTA` | `y` | Enable FOTA |
| `CONFIG_DOWNLOADER_MAX_FILENAME_SIZE` | `2048` | Required: URLs are ~1700 chars |
| `CONFIG_DOWNLOADER_STACK_SIZE` | `2048` | Downloader thread stack |

> **Important:** `CONFIG_DOWNLOADER_MAX_FILENAME_SIZE=2048` is required.
> The default (255 bytes) truncates the signed URL, causing HTTP 403.

---

## Build and flash

FOTA requires MCUboot (dual-bank layout). It is auto-selected when
`CONFIG_CONEXIO_CLOUD_FOTA=y`.

```bash
# Build v1.0.0 (initial)
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash

# Build v1.1.0 (update) — change VERSION file first:
#   PATCHLEVEL = 1
west build -b conexio_stratus_pro/nrf9151/ns --pristine

# Sign the update image (required by MCUboot)
west sign -t imgtool --no-hex -B update.bin -- \
  --key <bootloader-signing-key.pem>

# Upload update.bin to Conexio Console → Firmware → Upload
```

See `samples/app/FOTA_BUILD_REFERENCE.md` for full signing instructions.

---

## How to trigger an update

1. Build and upload `v1.1.0` binary to Conexio Console → Firmware
2. Click **Create Job** → select the device → click **Send**
3. The device receives `FIRMWARE_UPDATE` at the next MQTT poll

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio FOTA Sample ===
[00:00:00.257] <inf> app: Current firmware: v1.0.0
[00:00:19.533] <inf> mqtt_transport: MQTT connected
[00:00:19.787] <inf> app: Connected — 355025934980275 | App v1.0.0 | SDK v2.3.0
...
[00:05:10.100] <inf> fota: FOTA: download started — version: 1.1.0
[00:05:10.200] <inf> app: FOTA: download started — version: 1.1.0
[00:05:45.300] <inf> app: FOTA: 100%
[00:05:45.400] <inf> app: FOTA: download complete — rebooting to apply
...
*** Booting My Application v1.1.0 ***
[00:00:00.257] <inf> app: Current firmware: v1.1.0
[00:00:02.640] <inf> fota: FOTA: new firmware confirmed
```

---

## Maintenance window callback

Uncomment this in `main.c` to defer downloads until safe:

```c
static bool fota_safe_to_start(void) {
    return !motor_is_running();   // don't update while motor is active
}
conexio_cloud_set_fota_can_start_cb(fota_safe_to_start);
```

The SDK retries every `CONFIG_FOTA_PAUSE_RETRY_SEC` seconds (default: 60s).
