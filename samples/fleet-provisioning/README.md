# Fleet Provisioning Sample — Conexio Console

> **One firmware binary for your entire fleet.**
> No per-device certificates. No per-device builds.
> Each device provisions itself automatically on first boot.

## How it works

```
First boot                          Every subsequent boot
──────────────────────────────────  ──────────────────────────────
1. Embed claim cert in firmware     1. Check modem for device cert
2. Boot → connect LTE               2. Boot → connect LTE
3. Read IMEI → derive device ID     3. IMEI → device ID
4. MQTT connect (claim cert)        4. MQTT connect (device cert)
5. Request unique cert from AWS     5. Publish telemetry
6. Write cert to modem storage      6. Receive commands / config
7. Persist "done" flag in flash     (normal operation forever)
8. Reboot → normal operation
```

The device ID is derived automatically from the modem IMEI:
`IMEI <15-digit-IMEI>` — no `prj.conf` edit required.

---

## One-time AWS setup (done once by you, not by each user)

### Step 1 — Create the claim (bootstrap) certificate

This certificate is shared across all devices. It only has permission to
call the Fleet Provisioning API — nothing else.

```bash
# Create the claim cert
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile firmware/fleet-provisioning/src/certs/claim.crt \
  --private-key-outfile firmware/fleet-provisioning/src/certs/claim.key

# Note the certificateArn in the output
CLAIM_CERT_ARN="arn:aws:iot:us-east-1:ACCOUNT:cert/abc..."

# Attach the claim policy (created by deploy.sh)
aws iot attach-policy \
  --policy-name iot-dashboard-fleet-claim-policy \
  --target $CLAIM_CERT_ARN
```

### Step 2 — Download the AWS Root CA

```bash
curl -o firmware/fleet-provisioning/src/certs/AmazonRootCA1.pem \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

Your `src/certs/` should now contain:
```
src/certs/
├── AmazonRootCA1.pem   ← same for every AWS project
├── claim.crt           ← shared bootstrap cert (embed in all firmware)
└── claim.key           ← shared bootstrap key  (embed in all firmware)
```

### Step 3 — Create the Fleet Provisioning template in AWS IoT

The template tells AWS what to do when a device calls RegisterThing:
create a Thing, activate the cert, and attach the right policy.

```bash
# Create the device policy first (if not already done by deploy.sh)
aws iot create-policy \
  --policy-name iot-dashboard-device-telemetry-policy \
  --policy-document '{
    "Version": "2012-10-17",
    "Statement": [{
      "Effect": "Allow",
      "Action": [
        "iot:Connect",
        "iot:Publish",
        "iot:Subscribe",
        "iot:Receive"
      ],
      "Resource": "arn:aws:iot:us-east-1:*:*"
    }]
  }'

# Create the Fleet Provisioning template
aws iot create-provisioning-template \
  --template-name ConexioFleetTemplate \
  --enabled \
  --provisioning-role-arn arn:aws:iam::ACCOUNT:role/iot-dashboard-fleet-provisioning-role \
  --template-body '{
    "Parameters": {
      "SerialNumber": { "Type": "String" }
    },
    "Resources": {
      "thing": {
        "Type": "AWS::IoT::Thing",
        "Properties": {
          "ThingName": { "Ref": "SerialNumber" }
        }
      },
      "certificate": {
        "Type": "AWS::IoT::Certificate",
        "Properties": {
          "CertificateId": { "Ref": "AWS::IoT::Certificate::Id" },
          "Status": "Active"
        }
      },
      "policy": {
        "Type": "AWS::IoT::Policy",
        "Properties": {
          "PolicyName": "iot-dashboard-device-telemetry-policy"
        }
      }
    }
  }'
```

> **Tip:** If you use `./deploy.sh`, the CDK stack creates the template,
> IAM role, and both policies automatically. Skip this step if already deployed.

### Step 4 — Get your AWS IoT endpoint

```bash
aws iot describe-endpoint --endpoint-type iot:Data-ATS
# → abc123xyz.iot.us-east-1.amazonaws.com
```

---

## Building the firmware

### Edit prj.conf

Only two values to change — and they are the same for every device:

```
CONFIG_CONEXIO_AWS_BROKER_HOSTNAME="abc123xyz.iot.us-east-1.amazonaws.com"
CONFIG_CONEXIO_FLEET_TEMPLATE_NAME="ConexioFleetTemplate"
```

No device ID. No per-device certificate. One build for all.

### Build and flash

```bash
# From this directory (fleet-provisioning/)
west build -b nrf9160dk/nrf9160/ns
west flash
```

For Thingy:91:
```bash
west build -b thingy91/nrf9160/ns
west flash
```

---

## What happens when a user receives a device

1. User flashes the standard Conexio firmware (or receives a pre-flashed device)
2. Device powers on, connects to LTE
3. Fleet Provisioning runs automatically — takes ~5 seconds
4. Device reboots and starts normal MQTT operation
5. User opens the **Provisioning** page in the dashboard
6. Device appears listed by its IMEI-derived ID (e.g. `351358811234567`)
7. User clicks **Claim**, gives it a friendly name → appears in their fleet

The user never touches a certificate, CLI, or config file.

---

## Serial monitor output — first boot

```
=== Conexio Console Fleet Provisioning sample ===
Broker: abc123xyz.iot.us-east-1.amazonaws.com:8883
[settings] prov/done not found — first boot
[cert_store] Provisioning claim credentials...
[cert_store] Root CA (claim tag) provisioned (tag 10)
[cert_store] Root CA (device tag) provisioned (tag 20)
[cert_store] Claim certificate provisioned (tag 11)
[cert_store] Claim private key provisioned (tag 12)
[main] Connecting to LTE...
[main] LTE registered (home)
[main] Device ID derived from IMEI: 351358811234567
[main] Broker abc123xyz.iot.us-east-1.amazonaws.com resolved
[main] Device not yet provisioned — starting Fleet Provisioning...
[provision] Starting Fleet Provisioning for device: 351358811234567
[provision] Template: ConexioFleetTemplate
[provision] MQTT connected (provisioning session)
[provision] Subscribed to all provisioning response topics
[provision] Publishing CreateKeysAndCertificate request...
[provision] CreateKeysAndCertificate accepted — parsing response...
[provision] Received unique certificate and private key from AWS
[provision] Publishing RegisterThing request for device: 351358811234567
[provision] RegisterThing accepted — provisioning complete
[provision] AWS Thing name: 351358811234567
[cert_store] Writing unique device certificate (tag 21)...
[cert_store] Writing unique device private key (tag 22)...
[cert_store] Device credentials stored successfully
[provision] Fleet Provisioning completed successfully
[main] Provisioning flag persisted to flash
[main] Provisioning complete — rebooting to start normal operation
```

## Serial monitor output — subsequent boots

```
=== Conexio Console Fleet Provisioning sample ===
[settings] prov/done = 1
[cert_store] Claim credentials already in modem — skipping writes
[main] Device already provisioned — starting normal operation
[main] MQTT client configured with DEVICE credentials (tags 20/21/22)
[main] MQTT connected (normal operation) — device: 351358811234567
[main] Subscribed to: devices/351358811234567/commands
[main] Telemetry: temp=22.5 hum=61.0 rssi=-68
```

---

## MQTT topics (normal operation)

| Topic | Direction | Purpose |
|-------|-----------|---------|
| `devices/<imei-id>/telemetry` | Device → Cloud | Sensor readings |
| `devices/<imei-id>/commands`  | Cloud → Device | Commands, OTA config, schedules |

---

## Troubleshooting

**`CreateKeysAndCertificate REJECTED`**
- The claim certificate is not attached to a policy, or the policy does not allow `iot:CreateKeysAndCertificate`
- Run: `aws iot list-attached-policies --target $CLAIM_CERT_ARN`

**`RegisterThing REJECTED`**
- The Fleet Provisioning template name in `prj.conf` does not match what's in AWS
- The IAM role for the template is missing `iot:RegisterThing` permission
- Check AWS IoT Console → Connect → Fleet Provisioning

**Device does not appear in dashboard after provisioning**
- Check that the Provisioning Lambda hook (if used) is not rejecting the device
- Check AWS IoT Console → Things — the Thing should appear there first

**Re-provisioning a device (factory reset)**
```bash
# Wipe device credentials from modem (via RTT/UART shell or AT commands):
# AT%CMNG=3,21,1    ← delete device cert
# AT%CMNG=3,22,2    ← delete device key
# Also clear Settings flash, or hold a hardware button at boot to trigger re-provisioning
```
