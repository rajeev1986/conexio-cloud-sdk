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
`<15-digit-IMEI>` — no `prj.conf` edit required.

---

## One-time AWS setup (done once by you, not by each user)

All commands below are run from the repo root:
`cloudDeviceManagement/`

### Step 1 — Create the claim (bootstrap) certificate

This certificate is shared across all devices. It only has permission to
call the Fleet Provisioning API — nothing else.

```bash
# Create the claim cert and key
aws iot create-keys-and-certificate \
  --set-as-active \
  --certificate-pem-outfile conexio-cloud-sdk/samples/fleet-provisioning/src/certs/claim.crt \
  --private-key-outfile conexio-cloud-sdk/samples/fleet-provisioning/src/certs/claim.key

# Save the certificateArn from the output — you need it for the next command
CLAIM_CERT_ARN="arn:aws:iot:us-east-1:ACCOUNT:cert/abc..."
```

Now create the claim policy. This policy is intentionally narrow — it only
allows the device to reach the Fleet Provisioning MQTT topics.

> ⚠️ **Critical:** `iot:CreateKeysAndCertificate` must be listed as a
> separate `Action` alongside `iot:Publish`. AWS IoT treats it as a distinct
> permission — without it the connection is accepted but immediately closed,
> manifesting as an `ETIMEDOUT` disconnect on the device before any CONNACK
> exchange completes.

```bash
aws iot create-policy \
  --policy-name iot-dashboard-fleet-claim-policy \
  --policy-document '{
    "Version": "2012-10-17",
    "Statement": [
      {
        "Effect": "Allow",
        "Action": ["iot:Connect"],
        "Resource": "*"
      },
      {
        "Effect": "Allow",
        "Action": ["iot:Publish", "iot:Subscribe", "iot:Receive"],
        "Resource": [
          "arn:aws:iot:us-east-1:ACCOUNT:topic/$aws/certificates/create/*",
          "arn:aws:iot:us-east-1:ACCOUNT:topic/$aws/provisioning-templates/ConexioFleetTemplate/provision/*",
          "arn:aws:iot:us-east-1:ACCOUNT:topicfilter/$aws/certificates/create/*",
          "arn:aws:iot:us-east-1:ACCOUNT:topicfilter/$aws/provisioning-templates/ConexioFleetTemplate/provision/*"
        ]
      },
      {
        "Effect": "Allow",
        "Action": ["iot:CreateKeysAndCertificate"],
        "Resource": "*"
      }
    ]
  }'
```

Replace `ACCOUNT` with your 12-digit AWS account ID
(`aws sts get-caller-identity --query Account --output text`).

Then attach the policy to the claim certificate:

```bash
aws iot attach-policy \
  --policy-name iot-dashboard-fleet-claim-policy \
  --target $CLAIM_CERT_ARN
```

### Step 2 — Download the AWS Root CA

```bash
curl -o conexio-cloud-sdk/samples/fleet-provisioning/src/certs/AmazonRootCA1.pem \
  https://www.amazontrust.com/repository/AmazonRootCA1.pem
```

Your `src/certs/` should now contain these three files — and nothing else:

```
conexio-cloud-sdk/samples/fleet-provisioning/src/certs/
├── AmazonRootCA1.pem   ← same for every AWS project
├── claim.crt           ← shared bootstrap cert (embed in all firmware)
└── claim.key           ← shared bootstrap key  (KEEP SECRET — see note below)
```

> ⚠️ **Security:** `claim.key` is a shared secret embedded in every firmware
> binary. It must never be committed to git. Add these lines to your
> `.gitignore` if not already present:
> ```
> conexio-cloud-sdk/samples/fleet-provisioning/src/certs/claim.key
> conexio-cloud-sdk/samples/fleet-provisioning/src/certs/claim.crt
> conexio-cloud-sdk/samples/fleet-provisioning/src/certs/AmazonRootCA1.pem
> ```

### Step 3 — Create the IAM role for Fleet Provisioning

AWS IoT needs an IAM role to call the necessary IoT APIs on your behalf when
a device provisions itself. You must create this role before creating the
provisioning template.

```bash
# Create the trust policy document
cat > /tmp/fleet-trust-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Principal": { "Service": "iot.amazonaws.com" },
    "Action": "sts:AssumeRole"
  }]
}
EOF

# Create the role
aws iam create-role \
  --role-name iot-dashboard-fleet-provisioning-role \
  --assume-role-policy-document file:///tmp/fleet-trust-policy.json \
  --description "Allows AWS IoT Fleet Provisioning to create Things and manage certificates"

# Attach the permissions policy
cat > /tmp/fleet-provisioning-policy.json << 'EOF'
{
  "Version": "2012-10-17",
  "Statement": [{
    "Effect": "Allow",
    "Action": [
      "iot:CreateThing",
      "iot:DescribeThing",
      "iot:RegisterThing",
      "iot:AttachThingPrincipal",
      "iot:AttachPolicy",
      "iot:DescribeCertificate",
      "iot:UpdateCertificate",
      "iot:CreateKeysAndCertificate"
    ],
    "Resource": "*"
  }]
}
EOF

aws iam put-role-policy \
  --role-name iot-dashboard-fleet-provisioning-role \
  --policy-name FleetProvisioningPolicy \
  --policy-document file:///tmp/fleet-provisioning-policy.json
```

> **Note:** If you used `./deploy.sh` to deploy the CDK stack, the role and
> both policies are created automatically — skip Steps 1–3 and go straight
> to Step 4.

### Step 4 — Create the Fleet Provisioning template in AWS IoT

The template tells AWS what to do when a device calls `RegisterThing`:
create a Thing named after the IMEI, activate the unique cert, and attach
the device telemetry policy.

```bash
# Create the device telemetry policy first (if not already done by deploy.sh)
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

Replace `ACCOUNT` with your 12-digit AWS account ID.

### Step 5 — Get your AWS IoT endpoint

```bash
aws iot describe-endpoint --endpoint-type iot:Data-ATS
# → abc123xyz-ats.iot.us-east-1.amazonaws.com
```

---

## NCS v3.2.1 compatibility notes

These are breaking changes from earlier NCS versions that affect this sample.
All fixes are already applied in the source — this section documents them for
reference.

| Symbol / API | Change | Fix applied |
|---|---|---|
| `CONFIG_NET_SOCKETS_POSIX_NAMES` | Removed in NCS v2.x | Deleted from `prj.conf` |
| `CONFIG_NET_SECURITY` | Removed in NCS v2.x | Deleted from `prj.conf` |
| `settings h_set` callback | Signature changed from 5 args to 4 (dropped trailing `void *param`) | Updated in `main.c` |
| `mqtt_disconnect()` | Now requires a second `const struct mqtt_disconnect_param *param` argument | All call sites updated to `mqtt_disconnect(client, NULL)` |
| `getaddrinfo` / `freeaddrinfo` | POSIX aliases only available with `CONFIG_POSIX_API=y`; unnecessary for nRF91xx offloaded sockets | Replaced with `zsock_getaddrinfo` / `zsock_freeaddrinfo` |
| `modem_key_mgmt` before `nrf_modem_lib_init()` | Key management requires the modem AT pipe to be open | `nrf_modem_lib_init()` + `lte_lc_func_mode_set(OFFLINE)` called before `cert_store_provision_claim_creds()` |
| IMEI read via `modem_info_params_get()` | Returns zeroed IMEI when called before LTE registration because it fetches all params including network info | Replaced with `modem_info_string_get(MODEM_INFO_IMEI)` which issues `AT+CGSN` directly and works offline |

### Correct boot sequence (modem initialisation order matters)

```
1. settings_subsys_init()            load provisioning flag from flash
2. nrf_modem_lib_init()              start modem AT interface
   lte_lc_func_mode_set(OFFLINE)     stay offline during credential writes
3. cert_store_provision_claim_creds() write claim certs via AT%CMNG
4. modem_info_init()
   modem_info_string_get(IMEI)       read IMEI — works offline
5. reboot_counter_init()
6. lte_lc_connect_async()            go online for LTE
7. date_time_update_async()          NTP sync
8. zsock_getaddrinfo()               DNS resolve broker
```

If `modem_key_mgmt` is called before step 2, every write returns `-1` with
the log message `Failed to retrieve CMEE status, err -1`. This is not a
certificate problem — it means the modem library is not yet initialised.

---



### Edit prj.conf

Only two values to change — and they are the **same for every device in your
fleet**:

```
CONFIG_CONEXIO_AWS_BROKER_HOSTNAME="abc123xyz-ats.iot.us-east-1.amazonaws.com"
CONFIG_CONEXIO_FLEET_TEMPLATE_NAME="ConexioFleetTemplate"
```

No device ID. No per-device certificate. One build for all.

### Build and flash

```bash
# From the fleet-provisioning/ directory

# Conexio Stratus Pro (nRF9151) — non-secure partition
west build -b conexio_stratus_pro/nrf9151/ns

# nRF9160 DK
west build -b nrf9160dk/nrf9160/ns

# Thingy:91
west build -b thingy91/nrf9160/ns

west flash
```

> **Note for nRF9151 (Stratus Pro):** use the `conexio_stratus_pro/nrf9151/ns`
> board target, not `nrf9160dk/nrf9160/ns`. The nRF9151 has a different
> partition layout and the wrong target will build but the credentials write
> will fail at runtime.

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
Broker: abc123xyz-ats.iot.us-east-1.amazonaws.com:8883
[settings] prov/done not found — first boot
[cert_store] Provisioning claim credentials...
[cert_store] Root CA (claim tag) provisioned (tag 10)
[cert_store] Root CA (device tag) provisioned (tag 20)
[cert_store] Claim certificate provisioned (tag 11)
[cert_store] Claim private key provisioned (tag 12)
[main] Connecting to LTE...
[main] LTE registered (home)
[main] Device ID derived from IMEI: 351358811234567
[main] Broker abc123xyz-ats.iot.us-east-1.amazonaws.com resolved
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

**`modem_key_mgmt` returns `-1` / `Failed to retrieve CMEE status`**
- `nrf_modem_lib_init()` has not been called yet when `cert_store_provision_claim_creds()` runs
- The modem AT pipe is not open — key management cannot communicate with the modem
- Fix: ensure `nrf_modem_lib_init()` is called before any `modem_key_mgmt` call (see boot sequence above)

**Device ID reads `000000000000000`**
- `modem_info_params_get()` was used to fetch the IMEI — it returns a zeroed struct when the device is not yet registered on the network because it fetches all parameters including network info in one shot
- Fix: use `modem_info_string_get(MODEM_INFO_IMEI, ...)` which issues `AT+CGSN` directly and works before LTE registration

**MQTT disconnects immediately with `result -122` (ETIMEDOUT) during provisioning**
- The TLS handshake with AWS IoT completes but the connection is immediately closed
- Most common cause: the claim certificate policy is missing `iot:CreateKeysAndCertificate`
- This action must be listed explicitly — `iot:Publish` on the topic alone is not sufficient
- Verify and fix:
  ```bash
  # Check current policy
  aws iot get-policy --policy-name iot-dashboard-fleet-claim-policy \
    --query 'policyDocument' --output text

  # Add the missing action (creates a new policy version)
  aws iot create-policy-version \
    --policy-name iot-dashboard-fleet-claim-policy \
    --set-as-default \
    --policy-document '{
      "Version": "2012-10-17",
      "Statement": [
        {"Effect":"Allow","Action":["iot:Connect"],"Resource":"*"},
        {"Effect":"Allow","Action":["iot:Publish","iot:Subscribe","iot:Receive"],
         "Resource":[
           "arn:aws:iot:us-east-1:ACCOUNT:topic/$aws/certificates/create/*",
           "arn:aws:iot:us-east-1:ACCOUNT:topic/$aws/provisioning-templates/ConexioFleetTemplate/provision/*",
           "arn:aws:iot:us-east-1:ACCOUNT:topicfilter/$aws/certificates/create/*",
           "arn:aws:iot:us-east-1:ACCOUNT:topicfilter/$aws/provisioning-templates/ConexioFleetTemplate/provision/*"
         ]},
        {"Effect":"Allow","Action":["iot:CreateKeysAndCertificate"],"Resource":"*"}
      ]
    }'
  ```
- Second cause: the claim certificate is not **Active** in AWS IoT
  ```bash
  aws iot list-certificates --query 'certificates[*].{id:certificateId,status:status}'
  # If claim cert shows INACTIVE, activate it:
  aws iot update-certificate --certificate-id <id> --new-status ACTIVE
  ```

**Fleet Provisioning times out (3 minutes) without completing**
- Check AWS IoT Console → Activity for connection/authorisation errors
- Verify the provisioning template exists and is enabled:
  ```bash
  aws iot describe-provisioning-template --template-name ConexioFleetTemplate
  ```
- Check for orphaned `PENDING_ACTIVATION` certificates from failed attempts — they indicate `CreateKeysAndCertificate` succeeded but `RegisterThing` never completed:
  ```bash
  aws iot list-certificates --query 'certificates[?status==`PENDING_ACTIVATION`]'
  # Clean up orphans:
  aws iot update-certificate --certificate-id <id> --new-status REVOKED
  aws iot delete-certificate --certificate-id <id> --force-delete
  ```

**`CreateKeysAndCertificate REJECTED`**
- The claim certificate is not attached to `iot-dashboard-fleet-claim-policy`, or the policy is missing the required actions
- Verify: `aws iot list-attached-policies --target $CLAIM_CERT_ARN`
- Verify the policy document: `aws iot get-policy --policy-name iot-dashboard-fleet-claim-policy`

**`RegisterThing REJECTED`**
- The template name in `prj.conf` does not match what's in AWS (must be exactly `ConexioFleetTemplate`)
- The IAM role is missing one of the required actions (`iot:RegisterThing`, `iot:AttachPolicy`, etc.)
- Verify: `aws iam get-role-policy --role-name iot-dashboard-fleet-provisioning-role --policy-name FleetProvisioningPolicy`
- Check AWS IoT Console → Connect → Fleet Provisioning

**Device does not appear in dashboard after provisioning**
- The AWS Thing is created but hasn't been claimed yet — this is normal
- Verify the Thing exists: `aws iot describe-thing --thing-name 351358811234567`
- Open **Device Setup → Provisioning** in the dashboard and claim it

**Re-provisioning a device (factory reset)**
```bash
# Wipe device credentials from modem (via RTT/UART shell or AT commands):
# AT%CMNG=3,21,1    ← delete device cert (tag 21)
# AT%CMNG=3,22,2    ← delete device key  (tag 22)
# Also clear the Settings flash partition, or hold a hardware button at boot
# to trigger re-provisioning
```
