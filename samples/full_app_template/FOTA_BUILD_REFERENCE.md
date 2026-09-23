# FOTA & Build Reference — Conexio Stratus Pro

Reference for building, signing, and deploying firmware over-the-air to the
Conexio Stratus Pro (nRF9151 + TF-M) using the Conexio Cloud SDK.

---

## Table of Contents

1. [Flash partition layout (`pm_static.yml`)](#1-flash-partition-layout-pm_staticyml)
2. [MCUboot signing key](#2-mcuboot-signing-key)
3. [Building firmware](#3-building-firmware)
4. [Flashing via USB (first-time / key change)](#4-flashing-via-usb-first-time--key-change)
5. [FOTA end-to-end flow](#5-fota-end-to-end-flow)
6. [Important prj.conf settings for FOTA](#6-important-prjconf-settings-for-fota)
7. [Versioning](#7-versioning)
8. [Troubleshooting FOTA](#8-troubleshooting-fota)
9. [When to USB flash vs FOTA](#9-when-to-usb-flash-vs-fota)

---

## 1. Flash partition layout (`pm_static.yml`)

### What is it?

NCS uses **Partition Manager** to divide the flash into regions (MCUboot,
TF-M secure, NS application, secondary FOTA slot, NVS storage). Without
`pm_static.yml` this layout is recalculated **dynamically at every build**
based on current image sizes.

### Why does dynamic layout break FOTA?

MCUboot is flashed once (via USB) and stores the expected partition addresses
at compile time. If a later FOTA build shifts the primary/secondary slot
boundaries — because TF-M grew by 4 KB or MCUboot itself changed size —
MCUboot (from the old USB flash) doesn't know about the new addresses and
either reverts the swap or fails to boot the new image.

**`pm_static.yml` locks the partition layout permanently.** Every future
build — whether USB-flashed or FOTA'd — uses identical slot addresses.
MCUboot is always consistent with the app image.

### Current locked layout (nRF9151, NCS v3.2.1)

```
Address     Region                  Size
─────────────────────────────────────────────────────
0x00000     MCUboot bootloader      0x0C000  (48 KB)
0x0C000     (empty/pad)             0x04000
0x10000     mcuboot_pad (TF-M hdr)  0x00200
0x10200     TF-M secure image       0x07E00  (31.5 KB)
0x18000     NS application (app)    0x68000  (416 KB)
            ─────────────────────────────
            PRIMARY SLOT total:     0x70000  (448 KB)
            [0x10000 → 0x80000]
─────────────────────────────────────────────────────
0x80000     SECONDARY SLOT          0x70000  (448 KB)
            [FOTA downloads here]
            [0x80000 → 0xF0000]
─────────────────────────────────────────────────────
0xF0000     (empty)                 0x08000
0xF8000     NVS storage             0x06000
0xFE000     (empty)                 0x02000
0x100000    End of flash
─────────────────────────────────────────────────────
```

### NVS key allocation (used by SDK)

| Key ID | Usage |
|--------|-------|
| `0x0001` | Reboot counter (`_reboot_cnt` lifetime value) |
| `0x0002` | Reboot reason string |
| `0x0003` | Schedule watchdog active flag |
| `0x0004` | Schedule watchdog record (stop command + stopAt) |
| `0x0005` | FOTA pending SUCCEEDED (deviceId\njobId — survives reboot) |
| `0x0010–0x0012` | Offline buffer metadata (write/read index, count) |
| `0x2000+` | Offline buffer payload entries |

### Headroom limits

The current layout gives:
- **NS application** up to **416 KB** (0x68000)
- **TF-M secure** up to **31.5 KB** (0x7E00)

If your app grows beyond 416 KB or TF-M beyond 31.5 KB the build will fail
with a linker overflow error. At that point you need to update `pm_static.yml`
and do a one-time USB reflash cycle.

### How `pm_static.yml` was generated

```bash
# After a successful build, capture the current layout:
cp build/partitions.yml pm_static.yml
```

NCS automatically picks up `pm_static.yml` when it is in the application
source directory — no additional configuration needed.

---

## 2. MCUboot signing key

### Why a fixed key matters

MCUboot verifies every firmware image in the secondary slot using a public key
embedded at build time. The signed binary (`zephyr.signed.bin`) contains a
`KEYHASH` TLV that MCUboot compares against its embedded key. If they don't
match, MCUboot silently refuses to swap and boots the old image instead.

Changing the signing key requires:
1. Rebuilding MCUboot with the new key
2. Flashing MCUboot via USB
3. Uploading a binary signed with the new key

**Never change the key after shipping devices without a USB reflash.**

### Key file location

```
samples/app/keys/conexio-fota-signing.pem
```

This is a copy of the NCS default `root-ec-p256.pem` (ECDSA P-256).

### How the key is wired

`sysbuild.conf` passes it to `west sign` via `image_signing.cmake`:

```ini
SB_CONFIG_BOOT_SIGNATURE_KEY_FILE="/opt/nordic/ncs/v3.2.1/modules/lib/conexio-cloud-sdk/samples/app/keys/conexio-fota-signing.pem"
```

The path must be **absolute** — NCS resolves it from the west workspace root
and MCUboot's CMake does not double-prefix `SB_CONFIG_` variables.

### Verifying key consistency

After a build, run this to confirm the binary and MCUboot share the same key:

```bash
python3 - <<'EOF'
import sys, struct, hashlib, binascii, re
sys.path.insert(0, '/opt/nordic/ncs/v3.2.1/bootloader/mcuboot/scripts')
from imgtool import keys

base = '/opt/nordic/ncs/v3.2.1/modules/lib/conexio-cloud-sdk/samples/app'
k = keys.load(f'{base}/keys/conexio-fota-signing.pem')
expected = hashlib.sha256(k.get_public_bytes()).hexdigest()

with open(f'{base}/build/app/zephyr/zephyr.signed.bin', 'rb') as f:
    data = f.read()
hdr = struct.unpack_from('<H', data, 8)[0]
img = struct.unpack_from('<I', data, 12)[0]
off = hdr + img + 4
end = off + struct.unpack_from('<H', data, hdr + img + 2)[0]
while off < end:
    t, _, l = struct.unpack_from('<BBH', data, off)
    if t == 0x01:  # KEYHASH TLV
        kh = binascii.hexlify(data[off+4:off+4+l]).decode()
        print(f'Binary KEYHASH : {kh}')
        print(f'Expected       : {expected}')
        print(f'Match          : {"✅ OK" if kh == expected else "❌ MISMATCH"}')
    off += 4 + l

with open(f'{base}/build/mcuboot/zephyr/autogen-pubkey.c') as f:
    content = f.read()
hex_bytes = re.findall(r'0x([0-9a-f]{2})', content)
mcuboot_kh = hashlib.sha256(bytes(int(h,16) for h in hex_bytes)).hexdigest()
print(f'MCUboot key    : {mcuboot_kh}')
print(f'MCUboot match  : {"✅ OK" if mcuboot_kh == expected else "❌ MISMATCH"}')
EOF
```

All three lines must show `✅ OK` for FOTA to work.

---

## 3. Building firmware

### Prerequisites

- nRF Connect SDK v3.2.1 at `/opt/nordic/ncs/v3.2.1`
- Board: `conexio_stratus_pro/nrf9151/ns`
- Device provisioned with TLS credentials (tags 100/101/102)

### Build command

```bash
cd /opt/nordic/ncs/v3.2.1/modules/lib/conexio-cloud-sdk/samples/app

# Always do a pristine build when changing sysbuild.conf or key
# (delete build/ folder first — VS Code: right-click → Pristine Build)
west build -b conexio_stratus_pro/nrf9151/ns
```

Or in **nRF Connect for VS Code**: click **Pristine Build** on the application.

### What gets built

| Output file | Purpose |
|-------------|---------|
| `build/app/zephyr/zephyr.signed.bin` | Signed NS app + TF-M — **upload this to dashboard** |
| `build/merged.hex` | Full flash image (MCUboot + TF-M + app) — **flash this via USB** |
| `build/mcuboot/zephyr/zephyr.elf` | MCUboot ELF with embedded public key |
| `build/mcuboot/zephyr/autogen-pubkey.c` | Auto-generated from signing key |

### Bump version before each release build

Edit `VERSION` before building:

```
VERSION_MAJOR = 1
VERSION_MINOR = 0
PATCHLEVEL = 2    ← increment this
VERSION_TWEAK = 0
```

This embeds the version string in the binary (visible in boot logs and
reported as `_app_fw_version` in telemetry).

---

## 4. Flashing via USB (first-time / key change)

USB flash is required when:
- **First setup** on a new device
- **Signing key changes** (MCUboot must be rebuilt with the new key)
- **`pm_static.yml` layout changes** (partition sizes shifted)

```bash
# Flash the full merged image (MCUboot + TF-M + app)
west flash
```

Or use **newtmgr** for DFU over serial (MCUboot serial recovery):
```bash
newtmgr -c serial image upload build/app/zephyr/zephyr.signed.bin
newtmgr -c serial reset
```

After USB flash, save the signed binary:
```bash
cp build/app/zephyr/zephyr.signed.bin v1.0.0/zephyr.signed.bin
```

---

## 5. FOTA end-to-end flow

```
Dashboard (Firmware page)
  │
  │  1. Upload zephyr.signed.bin → S3
  │  2. Create Rollout Job
  │     ↓ Lambda publishes FIRMWARE_UPDATE to
  │     devices/{deviceId}/commands (MQTT QoS 1, 7-day presigned URL)
  │
  ▼
Device (firmware side)
  │
  │  3. Receives FIRMWARE_UPDATE command
  │     → fota_handle_command(jobId, jobDocument)
  │  4. fota.c parses presigned S3 URL
  │     → fota_download_start(host, file, CA_TAG=100, pdn=0, frag=0)
  │  5. Downloads binary to SECONDARY SLOT (0x80000–0xF0000)
  │  6. On download complete (FOTA_DOWNLOAD_EVT_FINISHED):
  │     a. Saves {deviceId}\n{jobId} to NVS key 0x0005
  │     b. Publishes IN_PROGRESS/installing to IoT Jobs topic
  │     c. Calls sys_reboot()
  │
  ▼
MCUboot (on reboot)
  │
  │  7. Reads secondary slot trailer — BOOT_SWAP_TYPE_TEST set
  │  8. Verifies ECDSA P-256 signature of secondary slot image
  │     (KEYHASH must match embedded public key from autogen-pubkey.c)
  │  9. Swaps primary ↔ secondary
  │
  ▼
New firmware (first boot)
  │
  │  10. fota_confirm() called from conexio_cloud_init() Step 11
  │      → boot_write_img_confirmed() — MCUboot won't revert
  │  11. "Firmware image confirmed (MCUboot rollback prevention)" logged
  │
  ▼
MQTT reconnects
  │
  │  12. fota_check_and_execute() reads NVS key 0x0005
  │      → Publishes SUCCEEDED to $aws/things/{id}/jobs/{jobId}/update
  │      → Clears NVS key 0x0005
  │
  ▼
Backend (auto-completion fallback)
  │
  │  13. Ingestion Lambda sees _app_fw_version in telemetry
  │      → If it matches a pending job → marks job Completed in DynamoDB
  │      → WebSocket push → Dashboard shows "Completed"
```

### What happens if MQTT is disconnected during download?

The modem radio is shared between the HTTPS download and the MQTT connection.
MQTT will disconnect — this is normal. The download continues uninterrupted
over the HTTPS socket. MQTT reconnects after the download finishes.

The `SUCCEEDED` publish is saved to NVS **before** the reboot so it survives
even if MQTT never reconnects before reboot.

### What if the device is offline when the job is created?

MQTT QoS 1 with `clean_session=0` (persistent session). The broker holds the
`FIRMWARE_UPDATE` command until the device reconnects. The presigned S3 URL
is valid for **7 days** — plenty of time for a device in PSM sleep.

---

## 6. Important `prj.conf` settings for FOTA

```ini
# MCUboot support
CONFIG_BOOTLOADER_MCUBOOT=y
CONFIG_IMG_MANAGER=y
CONFIG_MCUBOOT_IMG_MANAGER=y
CONFIG_DFU_TARGET=y
CONFIG_DFU_TARGET_MCUBOOT=y
CONFIG_STREAM_FLASH=y

# NCS downloader
CONFIG_DOWNLOADER=y
CONFIG_FOTA_DOWNLOAD=y
CONFIG_FOTA_DOWNLOAD_PROGRESS_EVT=y

# AWS presigned S3 URLs have ~1700-char query strings.
# Default filename size (255) truncates the URL → HTTP 403 from S3.
CONFIG_DOWNLOADER_MAX_FILENAME_SIZE=2048

# Downloader thread stack — increased because HTTPS+TLS uses more stack.
# Default (1280) causes stack overflow when cJSON/snprintf are called
# from the download callback thread.
CONFIG_DOWNLOADER_STACK_SIZE=2048

# FOTA download timeout
CONFIG_CONEXIO_CLOUD_FOTA_DOWNLOAD_TIMEOUT_SEC=300
```

---

## 7. Versioning

### VERSION file

```
samples/app/VERSION
```

```
VERSION_MAJOR = 1
VERSION_MINOR = 0
PATCHLEVEL = 1
VERSION_TWEAK = 0
EXTRAVERSION =
```

This produces `APP_VERSION_STRING = "1.0.1"` via the generated
`build/app/zephyr/include/generated/zephyr/app_version.h`.

The version is:
- Printed at boot: `<inf> app: App firmware : v1.0.1`
- Embedded in the MCUboot image header (verifiable with `python3` script above)
- Reported in telemetry as `_app_fw_version: "1.0.1"`
- Used by the dashboard to auto-complete FOTA jobs

### Bump version before every upload

**Always change the VERSION file before building the binary you upload to the
dashboard.** If you upload a binary with the same version as what's already
running, the dashboard job completion detection (via `_app_fw_version`) won't
fire.

Recommended versioning:
- `PATCHLEVEL` for bug fixes and minor changes
- `MINOR` for new features
- `MAJOR` for breaking SDK or partition layout changes

### Saved binary archives

Keep a copy of each released binary:

```
samples/app/
├── v1.0.0/zephyr.signed.bin   ← archived
├── v1.0.1/zephyr.signed.bin   ← archived
└── build/                     ← current build output
```

---

## 8. Troubleshooting FOTA

### Device receives command but download fails

**Symptom:** `fota_download_start failed (-22)` or `-7`

**Cause:** URL parsing issue. Check:
```
<dbg> fota: FOTA host: https://iot-dashboard-firmware-...amazonaws.com
<dbg> fota: FOTA file: firmware/.../zephyr.signed.bin?X-Amz-...
```
Both should be non-empty. `-22` = EINVAL (NULL file arg), `-7` = ENOENT (bad path).

---

### Download succeeds but device still runs old firmware

**Check 1 — Key mismatch (most common cause)**

MCUboot silently rejects the image if the KEYHASH doesn't match its embedded
public key. Run the verification script in [Section 2](#2-mcuboot-signing-key).

If there's a mismatch: rebuild with pristine build, USB flash, re-upload binary.

**Check 2 — MCUboot confirmation**

Look for this line in the boot log of the *new* firmware:
```
<inf> conexio_cloud: Firmware image confirmed (MCUboot rollback prevention)
```
If missing, `boot_write_img_confirmed()` failed. Check for errors in the log.

**Check 3 — VERSION not bumped**

If the new binary has the same version string as the current firmware, the
dashboard auto-completion fallback won't detect the update. Always bump VERSION.

**Check 4 — Wrong binary uploaded**

Verify the MCUboot header version in the S3 binary:
```bash
python3 -c "
import struct
with open('/tmp/downloaded.bin','rb') as f: d=f.read(28)
major,minor=struct.unpack_from('<BB',d,20)
rev=struct.unpack_from('<H',d,22)[0]
print(f'Image version: {major}.{minor}.{rev}')
"
```

---

### Dashboard job stuck at "In Progress" after successful FOTA

The `SUCCEEDED` publish happens on MQTT reconnect after reboot via NVS key
`0x0005`. If the device rebooted but didn't reconnect:
- Check serial logs for MQTT connection errors
- The ingestion Lambda fallback will catch it when the device publishes telemetry

To manually mark a job complete:
```bash
NOW=$(date -u +"%Y-%m-%dT%H:%M:%S.000Z")
aws dynamodb update-item \
  --table-name iot-dashboard-firmware \
  --key '{"pk":{"S":"JOB#<jobId>"},"sk":{"S":"META"}}' \
  --update-expression 'SET #s = :s, progress = :p, successCount = :sc, updatedAt = :ua' \
  --expression-attribute-names '{"#s":"status"}' \
  --expression-attribute-values "{\":s\":{\"S\":\"completed\"},\":p\":{\"N\":\"100\"},\":sc\":{\"N\":\"1\"},\":ua\":{\"S\":\"$NOW\"}}"
```

---

### Stack overflow during download

**Symptom:** `USAGE FAULT — Stack overflow (context area not valid)` at ~0%

**Cause:** `job_status_publish()` was using cJSON (heap allocation) on the
1280-byte downloader thread stack.

**Fix (already applied):** `job_status_publish()` uses `snprintf` only — no
heap. Also set `CONFIG_DOWNLOADER_STACK_SIZE=2048` in `prj.conf`.

---

### MQTT disconnects during download

**Symptom:** `MQTT disconnected (result -128)` at various % during download

**This is normal.** The nRF9151 modem shares its radio between the HTTPS
download socket and the MQTT TCP connection. The broker drops MQTT when no
PINGREQ arrives. The download continues uninterrupted and MQTT reconnects
after it finishes.

The firmware does **not** publish progress to IoT Jobs during download for
exactly this reason — the radio is busy.

---

## 9. When to USB flash vs FOTA

| Situation | USB flash required? |
|-----------|-------------------|
| First firmware on a new device | ✅ Yes (always) |
| Signing key changed | ✅ Yes |
| `pm_static.yml` partition layout changed | ✅ Yes |
| Regular firmware update (same key, same layout) | ❌ No — use FOTA |
| Bug fix / feature update | ❌ No — use FOTA |
| TF-M size grew beyond 31.5 KB | ✅ Yes (update pm_static.yml first) |

### USB flash command

```bash
# Flash full merged image (MCUboot + TF-M + app in one step)
west flash

# Alternatively, upload app only via serial (MCUboot serial recovery)
newtmgr -c serial image upload build/app/zephyr/zephyr.signed.bin
newtmgr -c serial reset
```

### After USB flash — save and upload the binary

```bash
# Archive the binary
cp build/app/zephyr/zephyr.signed.bin v1.0.X/zephyr.signed.bin

# Upload to Conexio Console → Firmware → Upload Version
# The binary's MCUboot version header is auto-detected by the dashboard
```

---

*Last updated: 2026-08-22*
*Board: conexio_stratus_pro/nrf9151/ns*
*NCS: v3.2.1*
*SDK branch: ncs-v3.2.1-port*
