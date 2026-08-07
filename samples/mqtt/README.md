# MQTT Sample — Conexio Console

Connects a Conexio Stratus Pro (nRF9151) to AWS IoT Core over MQTT/TLS and:

- Publishes a full telemetry payload on every boot, then on a configurable interval
- Collects **20 cellular and device health metrics** automatically from the modem
- Subscribes to `devices/<id>/commands` and `devices/<id>/config` for live control
- Handles commands (FAN_ON, REBOOT, SET_INTERVAL, etc.) and OTA config pushes
- **Option B power strategy** — disconnects after each publish and reconnects only
  when the next publish is due, minimising radio-on time for battery-powered devices

---

## Telemetry metrics

### Every publish
| Metric | Description |
|--------|-------------|
| `temperature` | Sensor reading °C (simulated; replace with real driver) |
| `humidity` | Sensor reading % (simulated; replace with real driver) |
| `_rssi` | RSRP signal strength in dBm |
| `_snr` | Signal-to-Noise Ratio index |
| `_modem_temp` | Modem die temperature °C |
| `_battery_mv` | Battery voltage in mV |
| `_tx_kb` | Kilobytes transmitted since boot |
| `_rx_kb` | Kilobytes received since boot |
| `_conn_loss` | LTE drop + re-register count since boot |
| `_reboot_cnt` | NVS-persisted boot counter (Fleet Health → Reboot Tracking) |

### Boot-once (first publish after each reboot)
| Metric | Description |
|--------|-------------|
| `_reboot_reason` | Reset cause: `watchdog`, `lockup`, `brownout`, `software`, `pin`, `por`, `wake` |
| `_sdk_version` | SDK version string (e.g. `"3.2.1"`) |
| `_modem_fw` | Modem firmware version (e.g. `"mfw_nrf9160_1.3.2"`) |
| `_operator` | Network operator name (e.g. `"AT&T"`) |
| `_lte_mode` | Active radio mode: `7` = LTE-M, `9` = NB-IoT |
| `_lte_connect_ms` | Time from boot to first LTE registration in ms |
| `_psm_tau_sec` | PSM TAU interval granted by network (-1 if not granted) |
| `_psm_active_sec` | PSM active window granted by network |
| `_edrx_ms` | eDRX interval in ms (0 if not granted) |

### Slow (every 4 publishes ≈ every 2h at 30-min interval)
| Metric | Description |
|--------|-------------|
| `_lte_band` | Active LTE band number (e.g. 20) |
| `_cell_id` | E-UTRAN cell ID (decimal) |
| `_tac` | Tracking Area Code |

---

## Setup

### 1. Create AWS IoT credentials

```bash
CERT_DIR="src/certs"

# Create certificate + private key and write directly to the certs directory
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile ${CERT_DIR}/device.crt \
  --private-key-outfile     ${CERT_DIR}/device.key

# Save the certificateArn from the output
CERT_ARN="arn:aws:iot:us-east-1:ACCOUNT:cert/abc..."

# Attach the device telemetry policy (created by deploy.sh)
aws iot attach-policy \
  --policy-name iot-dashboard-device-telemetry-policy \
  --target $CERT_ARN

# Create a Thing and attach the certificate
aws iot create-thing --thing-name stratus-pro-001
aws iot attach-thing-principal \
  --thing-name stratus-pro-001 \
  --principal  $CERT_ARN

# Get your AWS IoT endpoint
aws iot describe-endpoint --endpoint-type iot:Data-ATS
# → abc123xyz.iot.us-east-1.amazonaws.com
```

### 2. Download the AWS Root CA

```bash
curl -o src/certs/AmazonRootCA1.pem \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

### 3. Confirm src/certs/ contains three files

```
src/certs/
├── AmazonRootCA1.pem   ← AWS root CA (same for every AWS project)
├── device.crt          ← certificate PEM from step 1
└── device.key          ← private key from step 1
```

`CMakeLists.txt` embeds these at build time using `generate_inc_file_for_target`.
Certificates are written to the modem on first boot and skipped on subsequent boots.

### 4. Edit prj.conf

```
CONFIG_CONEXIO_DEVICE_ID="stratus-pro-001"
CONFIG_CONEXIO_AWS_BROKER_HOSTNAME="abc123xyz.iot.us-east-1.amazonaws.com"
```

### 5. Build and flash

```bash
west build --build-dir build \
  . --pristine --board conexio_stratus_pro/nrf9151/ns \
  -- -DBOARD_ROOT=/opt/nordic/ncs/v3.2.1/conexio-firmware-sdk

west flash
```

---

## Interval and keepalive configuration

```
CONFIG_CONEXIO_TELEMETRY_INTERVAL_SEC=30     # publish every 30 seconds
CONFIG_CONEXIO_MQTT_KEEPALIVE_SEC=1200       # AWS IoT maximum (20 min)
```

| Use case | `TELEMETRY_INTERVAL_SEC` | Notes |
|---|---|---|
| Development / testing | `10`–`60` | Rapid feedback |
| Overnight battery test | `1800` | 48 publishes/night |
| Production | `1800`–`3600` | Maximise battery life |

**Keepalive note:** AWS IoT Core maximum is **1200s (20 min)**. With
Option B (disconnect after publish), the keepalive only applies during the
brief ~10s connect→publish window, so its value has no effect on battery life.

---

## Power strategy — Option B (disconnect after publish)

The firmware uses Option B for battery-powered devices:

```
Boot
 └─ LTE connect (~5s)
 └─ NTP sync
 └─ DNS resolve
 └─ MQTT connect + TLS handshake (~5s)
 └─ Subscribe
 └─ Publish (boot metrics + sensors)
 └─ MQTT disconnect          ← radio goes idle
 └─ sleep(interval - 15s)    ← ~0s at 10s, 15s at 30s, 1785s at 1800s
 └─ MQTT reconnect + publish
 └─ disconnect → sleep → repeat
```

The LTE radio is active for **~15–20 seconds per interval** rather than
continuously. At a 30-minute interval this reduces radio-on time by ~98%.

---

## MQTT topics

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `devices/<id>/telemetry` | Device → Cloud | Sensor + modem metrics |
| `devices/<id>/commands` | Cloud → Device | Commands + scheduled actions |
| `devices/<id>/config` | Cloud → Device | OTA config pushes |
| `devices/<id>/commands/ack` | Device → Cloud | Command execution ACK |
| `devices/<id>/config/ack` | Device → Cloud | Config applied ACK |

---

## Telemetry payload example

```json
{
  "deviceId": "stratus-pro-001",
  "timestamp": "2026-08-01T20:15:32.000Z",
  "metrics": {
    "temperature": 24.7,
    "humidity": 63.2,
    "_rssi": -72,
    "_snr": 18,
    "_modem_temp": 32,
    "_battery_mv": 3780,
    "_tx_kb": 3,
    "_rx_kb": 1,
    "_conn_loss": 0,
    "_reboot_cnt": 5,
    "_reboot_reason": "por",
    "_sdk_version": "3.2.1",
    "_modem_fw": "mfw_nrf9160_1.3.2",
    "_operator": "AT&T",
    "_lte_mode": 7,
    "_lte_connect_ms": 6301,
    "_lte_band": 20,
    "_cell_id": 129000449,
    "_tac": 52228
  }
}
```

---

## Incoming command format

```json
{
  "type": "command",
  "command": "FAN_ON",
  "commandId": "cmd-abc123",
  "sk": "CMD#2026-08-01T...",
  "payload": { "speed": 80 }
}
```

Supported commands out of the box: `REBOOT`, `FAN_ON`, `FAN_OFF`,
`SET_INTERVAL`, `CALIBRATE`.

## OTA config format

```json
{
  "type": "config",
  "version": 3,
  "configId": "cfg-xyz789",
  "config": { "telemetryIntervalSec": 1800 }
}
```

`telemetryIntervalSec` updates the publish interval at runtime without a reboot.
