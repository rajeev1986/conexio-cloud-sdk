# MQTT Sample — Conexio Console

Connects an nRF91xx device to AWS IoT Core over MQTT/TLS and:
- Publishes telemetry to `devices/<device-id>/telemetry` every 60 seconds
- Subscribes to `devices/<device-id>/commands` to receive commands from the dashboard
- Handles: commands (FAN_ON, REBOOT, etc.), OTA config pushes, scheduled commands

## Setup

### 1. Create AWS IoT credentials

```bash
# Create a certificate + private key
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile device.crt \
  --public-key-outfile device.pub.key \
  --private-key-outfile device.key

# Note the certificateArn in the output — you need it for step 3
CERT_ARN="arn:aws:iot:us-east-1:123456789:cert/abc..."

# Attach the IoT policy (created by deploy.sh)
aws iot attach-policy \
  --policy-name iot-dashboard-device-telemetry-policy \
  --target $CERT_ARN

# Create a Thing and attach the certificate
aws iot create-thing --thing-name my-nrf-device
aws iot attach-thing-principal \
  --thing-name my-nrf-device \
  --principal $CERT_ARN

# Get your IoT endpoint
aws iot describe-endpoint --endpoint-type iot:Data-ATS
# → abc123xyz.iot.us-east-1.amazonaws.com
```

### 2. Download the AWS Root CA

```bash
curl -o src/certs/AmazonRootCA1.pem \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

### 3. Place certificates in src/certs/

```
src/certs/
├── AmazonRootCA1.pem   ← downloaded above
├── device.crt          ← certificate-pem from step 1
└── device.key          ← private-key from step 1
```

The `CMakeLists.txt` embeds these at build time using `generate_inc_file_for_target`.

### 4. Edit prj.conf

Set your device ID and AWS endpoint:
```
CONFIG_CONEXIO_DEVICE_ID="my-nrf-device"
CONFIG_CONEXIO_AWS_BROKER_HOSTNAME="abc123xyz.iot.us-east-1.amazonaws.com"
```

### 5. Build and flash

```bash
# From this directory (mqtt/)
west build -b nrf9160dk/nrf9160/ns
west flash
```

For Thingy:91:
```bash
west build -b thingy91/nrf9160/ns
```

## MQTT topics used

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `devices/<id>/telemetry` | Device → Cloud | Sensor readings every 60s |
| `devices/<id>/commands` | Cloud → Device | Commands, config pushes, scheduled actions |

## Telemetry payload format

```json
{
  "deviceId": "my-nrf-device",
  "timestamp": "2026-06-03T10:30:00.000Z",
  "metrics": {
    "temperature": 22.5,
    "humidity": 61.2,
    "_rssi": -68
  }
}
```

## Incoming command format

```json
{
  "type": "command",
  "deviceId": "my-nrf-device",
  "command": "FAN_ON",
  "payload": { "speed": 80 },
  "source": "dashboard",
  "timestamp": "2026-06-03T10:30:00.000Z"
}
```

Config push (from OTA Config page):
```json
{
  "type": "config",
  "deviceId": "my-nrf-device",
  "config": { "telemetryIntervalSec": 30, "alertThreshold": 80 },
  "version": 3,
  "source": "ota-config"
}
```
