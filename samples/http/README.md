# HTTP Sample — Conexio Console

Sends telemetry to the Conexio Console over HTTPS POST.
Simpler than MQTT — no device certificates, just an API key.

**Limitation:** telemetry-only. Cannot receive commands, schedules,
or OTA config pushes from the dashboard. Use the MQTT sample for those.

## Setup

### 1. Get your API key

```bash
aws secretsmanager get-secret-value \
  --secret-id iot-dashboard/device-ingest-api-key \
  --query SecretString \
  --output text
```

### 2. Get your API Gateway URL

```bash
# From CDK outputs after deploy:
cat infra/cdk-outputs.json | python3 -c \
  "import json,sys; d=json.load(sys.stdin)['IotDashboardStack'];
   print(next(v for k,v in d.items() if 'HttpApiUrl' in k or 'HttpApi' in k))"
# → https://abc123.execute-api.us-east-1.amazonaws.com
```

### 3. Edit prj.conf

```
CONFIG_CONEXIO_DEVICE_ID="my-nrf-device"
CONFIG_CONEXIO_API_KEY="your-api-key-from-secrets-manager"
CONFIG_CONEXIO_API_HOST="abc123.execute-api.us-east-1.amazonaws.com"
```

### 4. Build and flash

```bash
west build -b nrf9160dk/nrf9160/ns
west flash
```

## Telemetry payload format

```json
{
  "deviceId": "my-nrf-device",
  "timestamp": "2026-06-03T10:30:00.000Z",
  "metrics": {
    "temperature": 22.5,
    "humidity": 61.2,
    "_rssi": -72
  }
}
```

POST to: `https://<api-host>/v1/ingest`
Header: `x-api-key: <your-api-key>`
