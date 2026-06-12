# Conexio Cloud SDK — Phase 2 (Zero Configuration)

> **No endpoint in firmware. No API key in firmware.**
> The only thing your application configures is `CONFIG_CONEXIO_CLOUD=y`.

Phase 2 extends Phase 1 by fetching the MQTT/HTTP endpoint, API key, and
Root CA certificate URL from the Conexio config service at runtime using the
device IMEI as its identity. The application code is identical to Phase 1.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        APPLICATION LAYER  (main.c)                          │
│                                                                             │
│   conexio_cloud_register_sensor("temperature", read_temp, dev)             │
│   conexio_cloud_register_command("FAN_ON", on_fan_on, NULL)                │
│   conexio_cloud_register_setting_int("alertThreshold", on_threshold, NULL) │
│   conexio_cloud_init(event_handler)    ←── one call, everything else       │
│   while(1) { k_sleep(...); }           ←── main loop just sleeps           │
└───────────────────────┬─────────────────────────────────────────────────────┘
                        │  Public API  (include/conexio_cloud/conexio_cloud.h)
┌───────────────────────▼─────────────────────────────────────────────────────┐
│                         SDK CORE  (conexio_cloud.c)                         │
│                                                                             │
│  ┌─────────────────┐  ┌──────────────────┐  ┌───────────────────────────┐  │
│  │  Command        │  │  Settings        │  │  Sensor Registry          │  │
│  │  Registry       │  │  Registry        │  │                           │  │
│  │  (up to 16)     │  │  (int/bool/float │  │  Callbacks invoked before │  │
│  │                 │  │   /string)       │  │  every publish            │  │
│  │  dispatch_      │  │  dispatch_       │  │  Return NaN to skip cycle │  │
│  │  command()      │  │  setting()       │  └───────────────────────────┘  │
│  └─────────────────┘  └──────────────────┘                                 │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  Background Thread  (cloud_thread_fn)                                │  │
│  │                                                                      │  │
│  │  loop:                                                               │  │
│  │    1. kick watchdog                                                  │  │
│  │    2. PSM wake (if sleeping)                                         │  │
│  │    3. reconnect if disconnected  ──► transport_connect()            │  │
│  │    4. transport_poll()           ──► drive MQTT event loop          │  │
│  │    5. interval elapsed?                                              │  │
│  │         └─ has app data? (sensors registered OR metric queued)      │  │
│  │               ├─ NO  → skip, advance timer, log "metrics held back" │  │
│  │               └─ YES → conexio_cloud_publish()                      │  │
│  │                           └─ build_payload()                        │  │
│  │                                ├─ BOOT-ONCE: _reboot_cnt/reason,   │  │
│  │                                │  _lte_connect_ms, _lte_mode,      │  │
│  │                                │  _psm timers, _sdk_version,       │  │
│  │                                │  _modem_fw, _operator             │  │
│  │                                ├─ SLOW (every N): _lte_band,       │  │
│  │                                │  _cell_id, _tac                   │  │
│  │                                ├─ EVERY: _rssi, _snr, _conn_loss,  │  │
│  │                                │  _tx_kb, _rx_kb, _modem_temp      │  │
│  │                                ├─ sensor callbacks (app sensors)   │  │
│  │                                └─ metric queue (app metrics)       │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  ┌──────────────────────────────────────────────────────────────────────┐  │
│  │  Boot sequence inside conexio_cloud_init()                          │  │
│  │                                                                      │  │
│  │  1. reboot_counter_init()   NVS: read+increment _reboot_cnt         │  │
│  │  2. reboot_reason_init()    hwinfo: read RESETREAS → _reboot_reason │  │
│  │  3. modem_info_init()       IMEI → device ID (bare 15-digit IMEI)  │  │
│  │  4. connectivity_stats_init() enable AT%%XCONNSTAT for tx/rx        │  │
│  │  5. retry_init()            arm hardware watchdog                   │  │
│  │  6. offline_buffer_init()   mount NVS flash ring buffer             │  │
│  │  7. register built-in cmds  REBOOT, SET_INTERVAL, FIRMWARE_UPDATE  │  │
│  │  8. conexio_lte_connect()   block until LTE registered              │  │
│  │  9. NTP sync                date_time_update_async() + semaphore    │  │
│  │  10. config_fetch()         GET config.conexio.io?imei=<IMEI>      │  │
│  │  11. cert_store_provision() download Root CA, verify device creds  │  │
│  │  12. transport_init()       set broker host, build topic strings    │  │
│  │  13. power_mgr_init()       request PSM/eDRX with network          │  │
│  │  14. fota_init()            confirm MCUboot image if post-OTA boot  │  │
│  │  15. k_thread_create()      spawn background thread                 │  │
│  └──────────────────────────────────────────────────────────────────────┘  │
└───────────────────────┬─────────────────────────────────────────────────────┘
                        │  Internal transport interface  (transport.h)
          ┌─────────────┴──────────────┐
          │                            │
┌─────────▼──────────┐    ┌────────────▼────────────┐
│  MQTT Transport    │    │   HTTP Transport         │
│  (mqtt_transport.c)│    │   (http_transport.c)     │
│                    │    │                          │
│  TLS port 8883     │    │  HTTPS port 443          │
│  AWS IoT Core      │    │  API Gateway             │
│                    │    │                          │
│  TX: telemetry     │    │  TX: POST /v1/ingest     │
│  RX: commands      │    │  RX: GET /v1/commands    │
│  RX: config pushes │    │  RX: GET /v1/config/     │
│  TX: command ACKs  │    │       pending            │
│  TX: config ACKs   │    │  TX: PUT cmd/config ACKs │
│                    │    │                          │
│  Real-time cmds ✓  │    │  Polled after each POST  │
│  Bidirectional ✓   │    │  One-way + poll ✓        │
└────────────────────┘    └──────────────────────────┘
                        │
         ┌──────────────┼───────────────────┐
         │              │                   │
┌────────▼──────┐ ┌─────▼──────┐  ┌────────▼───────┐
│  Optional SDK │ │  Optional  │  │  Optional SDK  │
│  Modules      │ │  SDK Module│  │  Module        │
│               │ │            │  │                │
│  power_mgr.c  │ │ offline_   │  │  retry.c       │
│               │ │ buffer.c   │  │                │
│  Negotiates   │ │            │  │  Exp. backoff  │
│  PSM/eDRX     │ │  NVS flash │  │  + HW watchdog │
│  with network │ │  ring buf  │  │                │
│               │ │  for       │  │  Reboots after │
│  Saves 5-20x  │ │  offline   │  │  max failures  │
│  battery life │ │  payloads  │  │  to recover    │
└───────────────┘ └────────────┘  └────────────────┘
         │
┌────────▼──────┐ ┌────────────┐
│  Optional SDK │ │  Always    │
│  Module       │ │  Active    │
│               │ │            │
│  fota.c       │ │  lte.c     │
│               │ │            │
│  AWS IoT Jobs │ │  LTE conn  │
│  + MCUboot    │ │  + passive │
│  dual-bank    │ │  session   │
│  OTA updates  │ │  metrics   │
└───────────────┘ └────────────┘
                        │
┌───────────────────────▼─────────────────────────────────────────────────────┐
│                    ZEPHYR RTOS  +  NRF CONNECT SDK                          │
│                                                                             │
│   lte_lc  │  modem_info  │  fota_download  │  nvs  │  hwinfo  │  date_time │
└───────────────────────┬─────────────────────────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────────────────────────┐
│                    nRF9160 SiP                                               │
│                                                                             │
│   Cortex-M33 Application Core   │   LTE modem (mfw_nrf9160_x.x.x)          │
│   256 KB RAM / 1 MB Flash        │   LTE-M / NB-IoT / GPS                   │
│   Internal flash (NVS storage)   │   Modem NVM (TLS certs, tags 98-102)     │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Module responsibilities

| Module | File | Enabled by | Responsibility |
|--------|------|-----------|----------------|
| **SDK Core** | `conexio_cloud.c` | `CONFIG_CONEXIO_CLOUD=y` | Command/setting/sensor dispatch, payload builder, background thread, boot sequence |
| **LTE** | `lte.c` | Always (part of core) | LTE connection + passive session metrics collector |
| **Config Fetch** | `config_fetch.c` | Always (part of core) | Fetch MQTT/HTTP endpoints from Conexio config service at runtime |
| **Cert Store** | `cert_store.c` | Always (part of core) | Download Root CA, verify device cert+key in modem NVM |
| **MQTT Transport** | `mqtt_transport.c` | `CONFIG_CONEXIO_CLOUD_MQTT=y` | TLS MQTT to AWS IoT Core, subscribe commands+config, publish ACKs |
| **HTTP Transport** | `http_transport.c` | `CONFIG_CONEXIO_CLOUD_HTTP=y` | HTTPS POST telemetry, poll commands, poll pending config |
| **Power Manager** | `power_mgr.c` | `CONFIG_CONEXIO_CLOUD_PSM=y` | Negotiate PSM/eDRX, modem temperature, wake/sleep control |
| **Offline Buffer** | `offline_buffer.c` | `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y` | NVS flash ring buffer, replay on reconnect |
| **Retry** | `retry.c` | `CONFIG_CONEXIO_CLOUD_RETRY=y` | Exponential backoff + jitter, hardware watchdog |
| **FOTA** | `fota.c` | `CONFIG_CONEXIO_CLOUD_FOTA=y` | AWS IoT Jobs integration, MCUboot dual-bank OTA |

### Data flow — telemetry publish

```
Sensor read callbacks          Application send_metric()
        │                               │
        ▼                               ▼
   sensor_registry[]              metric_queue[]
        │                               │
        └───────────────┬───────────────┘
                        │  (both checked: has_app_data guard)
                        ▼
                  build_payload()
                        │
                ┌───────┴────────┐
                │  SDK metrics   │  (three-tier: boot-once / slow / every)
                │  App metrics   │  (sensors + queued)
                └───────┬────────┘
                        │  JSON string
                        ▼
               transport_publish()
                        │
           ┌────────────┴────────────┐
           ▼                         ▼
    mqtt_publish()            POST /v1/ingest
    (AWS IoT Core)            (API Gateway)
                                     │
                              GET /v1/commands
                              GET /v1/config/pending
```

### Boot sequence timeline

```
t=0      Power on / reboot
         │
t~10ms   reboot_counter_init()   — NVS: _reboot_cnt++
         reboot_reason_init()    — hwinfo RESETREAS → _reboot_reason
         │
t~50ms   modem_info_init()       — IMEI read → device ID
         connectivity_stats_init()
         │
t~100ms  retry_init()            — WDT armed
         offline_buffer_init()   — NVS mounted
         register built-ins      — REBOOT, SET_INTERVAL, FIRMWARE_UPDATE
         │
t~1.2s   lte_lc_connect_async()  — modem starts scanning
         │
         ┌── lte_evt_handler() fires on events:
         │   CELL_UPDATE   → _cell_id, _tac
         │   LTE_MODE      → _lte_mode
         │   PSM_UPDATE    → _psm_tau_sec, _psm_active_sec
         │   NW_REG_STATUS → registered! → records _lte_connect_ms
         │
t~5s     LTE registered           — semaphore released
         │
t~6s     NTP sync                 — date_time_update_async()
         │
t~7s     config_fetch()           — GET config.conexio.io (HTTPS)
         cert_store_provision()   — download Root CA if absent
         transport_init()         — set broker host + topic strings
         power_mgr_init()         — request PSM/eDRX
         fota_init()              — confirm MCUboot image if OTA boot
         │
t~8s     background thread spawned
         transport_connect()      — TCP+TLS to MQTT broker or HTTP ready
         │
t~9s     CONNECTED
         SDK READY — first publish waits for application data
```

## What your application looks like

**`prj.conf` — the complete cloud configuration:**
```ini
CONFIG_CONEXIO_CLOUD=y
CONFIG_CONEXIO_CLOUD_MQTT=y
CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=60

# Optional production features — all provided by the SDK, zero application code:
CONFIG_CONEXIO_CLOUD_PSM=y               # LTE PSM power saving (~2-3 µA sleep)
CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y    # Buffer telemetry in flash when offline
CONFIG_CONEXIO_CLOUD_FOTA=y              # OTA firmware updates via AWS IoT Jobs
CONFIG_CONEXIO_CLOUD_RETRY=y             # Exponential backoff + hardware watchdog
```

That's it. No host. No port. No API key. One binary for your entire fleet,
regardless of which Conexio Console deployment it belongs to.

**`main.c` — identical to Phase 1:**
```c
#include <conexio_cloud/conexio_cloud.h>

void main(void) {
    conexio_cloud_init(my_event_handler);
    while (1) {
        conexio_cloud_send_metric("temperature", read_temperature());
        k_sleep(K_SECONDS(CONFIG_CONEXIO_CLOUD_INTERVAL_SEC));
    }
}
```

## How it works

```
Device boots
     │
     ├─ Connect to LTE
     │
     ├─ GET https://config.conexio.io/v1/device-config?imei=<IMEI>
     │       ↓
     │   Response (workspace-specific, tied to IMEI registration):
     │   {
     │     "mqttHost":  "abc123.iot.us-east-1.amazonaws.com",
     │     "httpHost":  "abc123.execute-api.us-east-1.amazonaws.com",
     │     "httpApiKey": "your-api-key",
     │     "rootCaUrl": "https://www.amazontrust.com/repository/AmazonRootCA1.pem"
     │   }
     │
     ├─ Download + provision Root CA from rootCaUrl (once, cached in modem)
     ├─ Verify device cert + key present in modem (from fleet provisioning)
     │
     └─ Connect to mqttHost and start normal operation
          │
          ├─ Publish telemetry every interval
          └─ Receive commands / OTA config from dashboard
```

Config is cached for `CONFIG_CONEXIO_CLOUD_CONFIG_CACHE_SEC` (default: 1 hour).
If your backend URL ever changes, devices pick it up automatically at next cache expiry.

## Setup

### 1. Deploy the backend

```bash
./deploy.sh --no-vpc
```

### 2. Deploy the Conexio config service

You need a small endpoint at `config.conexio.io/v1/device-config` (or your
own domain) that:

- Accepts `GET /v1/device-config?imei=<15-digit-IMEI>`
- Returns the JSON config for the workspace that owns that IMEI
- Requires registration of IMEIs to workspaces (done via the dashboard)

The config service Lambda is included in the backend:
`lambdas/src/provisioning/handler.ts` — the device config endpoint is wired
automatically when you deploy.

### 3. Register the device IMEI to your workspace

On the **Provisioning** page in the Conexio Console dashboard, register the
IMEI (either via USB/Serial commissioning or manual entry). This tells the
config service which workspace the device belongs to.

### 4. Flash the firmware

No `prj.conf` endpoint edits. Just build and flash:

```bash
cd firmware/sdk-sample-app
# In CMakeLists.txt, uncomment conexio-cloud-sdk-v2 and comment out v1
west build -b nrf9160dk/nrf9160/ns
west flash
```

## Upgrading from Phase 1

1. In `CMakeLists.txt`, switch `ZEPHYR_EXTRA_MODULES` from `conexio-cloud-sdk`
   to `conexio-cloud-sdk-v2`
2. In `prj.conf`, remove `CONFIG_CONEXIO_CLOUD_MQTT_HOST`,
   `CONFIG_CONEXIO_CLOUD_HTTP_HOST`, and `CONFIG_CONEXIO_CLOUD_HTTP_API_KEY`
3. No `main.c` changes — the public API is identical

## API reference

Same as Phase 1. See `include/conexio_cloud/conexio_cloud.h`.

## Certificate handling

Phase 2 manages certificates entirely automatically:
- **Root CA** — downloaded from AWS Trust Services URL returned by config service
- **Device cert + key** — must be provisioned by fleet provisioning (first boot)

No certificate files in your project. No `src/certs/` directory needed.

---

## Device Observability Metrics

The SDK automatically collects and publishes a comprehensive set of device
health and network metrics with every telemetry payload. No application code
required — they appear in `metrics` alongside your sensor data.

All SDK-managed metrics use the `_` prefix to distinguish them from
application metrics (e.g. `temperature`, `humidity`).

### Complete metrics reference

The SDK uses a three-tier publish strategy to minimise data usage:

| Tier | When sent | Metrics |
|------|-----------|---------|
| **Boot-once** | First payload after each boot only | `_reboot_cnt`, `_reboot_reason`, `_lte_connect_ms`, `_lte_mode`, `_psm_tau_sec`, `_psm_active_sec`, `_edrx_ms`, `_edrx_ptw_ms`, `_sdk_version`, `_modem_fw`, `_operator` |
| **Slow** | Every `CONFIG_CONEXIO_CLOUD_SLOW_METRIC_INTERVAL` publishes (default 10) | `_lte_band`, `_cell_id`, `_tac` |
| **Every publish** | Every payload | `_rssi`, `_snr`, `_conn_loss`, `_reset_loop`, `_tx_kb`, `_rx_kb`, `_modem_temp`, `_battery_mv`, application sensors |

The cloud must store the last-seen value of boot-once and slow metrics
per device and use them for display when absent from recent payloads.

#### SDK metrics never cause a wakeup

SDK metrics are **passive passengers** — they only go out when the
application has sensor or application data to send. The SDK will never
wake the radio, open an LTE connection, or publish a payload that
contains only SDK metrics.

A publish is skipped entirely if:
- No sensor callbacks are registered (`conexio_cloud_register_sensor()`), **and**
- No metrics are queued via `conexio_cloud_send_metric()`

This means `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` is a **ceiling**, not a
floor. For a device that reads sensors every 5 minutes, set it to 300.
The SDK fires at most every 300 s, reads your sensor callbacks, and
publishes. It will not fire an extra publish just to transmit `_rssi`.

#### Application publish patterns

The SDK supports three patterns for getting application data into payloads.
Choose based on your use case — they can also be combined.

**Pattern 1 — sensor registration (recommended)**

Register sensor callbacks before `conexio_cloud_init()`. The SDK calls them
automatically before each publish. Your main loop just sleeps.

```c
static double read_temperature(void *arg) {
    struct sensor_value val;
    sensor_sample_fetch(arg);
    sensor_channel_get(arg, SENSOR_CHAN_AMBIENT_TEMP, &val);
    return sensor_value_to_double(&val);
    // Return NAN to skip this reading for this cycle (sensor not ready)
}

conexio_cloud_register_sensor("temperature", read_temperature, temp_dev);
conexio_cloud_register_sensor("humidity",    read_humidity,    hum_dev);

// SDK interval drives the publish rate — set it to your desired period
// CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=60  in prj.conf

int main(void) {
    conexio_cloud_register_sensor("temperature", read_temperature, temp_dev);
    conexio_cloud_init(NULL);
    while (1) {
        k_sleep(K_SECONDS(conexio_cloud_get_interval_sec()));
        // SDK background thread handles everything — nothing else needed here
    }
}
```

**Pattern 2 — manual queue from app thread**

Queue metrics from your own thread on your own schedule.
The SDK publishes them at the next interval tick.

```c
int main(void) {
    conexio_cloud_init(NULL);
    while (1) {
        float temp = read_temperature();
        float hum  = read_humidity();
        conexio_cloud_send_metric("temperature", temp);
        conexio_cloud_send_metric("humidity",    hum);
        k_sleep(K_SECONDS(60));   // your own cadence — must be <= INTERVAL_SEC
    }
}
```

> Note: if your sleep is longer than `INTERVAL_SEC`, the SDK fires but finds
> nothing queued and skips. Set `INTERVAL_SEC` ≥ your app sleep to ensure
> data is always waiting when the SDK fires.

**Pattern 3 — fully manual publish (interval = 0)**

Disable the background publish entirely and call `conexio_cloud_publish()`
yourself. Full control over timing — useful for event-driven devices.

```c
// prj.conf
CONFIG_CONEXIO_CLOUD_INTERVAL_SEC=0   // disables background publish

int main(void) {
    conexio_cloud_init(NULL);
    while (1) {
        // Wake from deep sleep, read sensors, queue metrics, publish, sleep
        conexio_cloud_send_metric("temperature", read_temperature());
        conexio_cloud_send_metric("humidity",    read_humidity());
        conexio_cloud_publish();   // SDK metrics automatically included
        enter_deep_sleep(300);
    }
}
```

**When is a publish skipped?**

The SDK skips a scheduled publish if neither condition is true:
- At least one sensor callback is registered, **or**
- At least one metric is queued via `conexio_cloud_send_metric()`

When skipped, the SDK logs at DEBUG level:
```
[DBG] conexio_cloud: Publish skipped — no application data queued (SDK metrics held back)
```
SDK metrics are held and will be included in the next publish that has
application data. The interval timer still advances normally so there is
no busy-loop.

#### Signal quality
Refreshed every `CONFIG_CONEXIO_CLOUD_MODEM_INFO_REFRESH` publishes (default: 5).

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_rssi` | number | dBm | RSRP (Reference Signal Received Power). Raw modem index. Convert: `idx < 0 → idx-140`, `idx > 0 → idx-141`. Range: -44 to -156 dBm. Good: > -100, Poor: < -120. |
| `_snr` | number | index | SNR index. Convert to dB: `value - 24`. E.g. index 39 = 15 dB. 127 = unavailable. Range 0–49 (maps to -24 dB to +25 dB). |

#### Radio/network context
Boot-once: `_lte_mode`. Slow (every N publishes): `_lte_band`, `_cell_id`, `_tac`, `_operator`.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_lte_mode` | number | enum | Active radio technology. `7` = LTE-M (Cat-M1), `9` = NB-IoT. Omitted if not yet determined. |
| `_lte_band` | number | integer | Active LTE frequency band (e.g. `3`, `20`, `28`). Useful for diagnosing carrier-specific coverage. `0` = unavailable. |
| `_cell_id` | number | integer | E-UTRAN Cell Identity (decimal). Changes when the device moves between cells. Can be used as a location proxy without GPS. Omitted if `0xFFFFFFFF` (invalid). |
| `_tac` | number | integer | Tracking Area Code. Changes when the device moves between tracking areas. Combined with `_cell_id` gives area-level location. Omitted if `0xFFFFFFFF` (invalid). |
| `_operator` | string | — | Mobile network operator name (e.g. `"Telia SE"`, `"AT&T"`). **Sent once — first publish after each boot only.** Operator never changes mid-session. |

#### Device identity
Boot-once. String values cached after first read, never change at runtime.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_modem_fw` | string | — | Modem firmware version string (e.g. `"mfw_nrf9160_1.3.6"`). **Sent once — first publish after each boot only.** The cloud stores the last-seen value per device. Lets you correlate issues to modem FW releases across the fleet. |
| `_sdk_version` | string | — | Conexio Cloud SDK semantic version (e.g. `"2.1.0"`). **Sent once — first publish after each boot only.** |

#### Device health
Published on every payload.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_reboot_cnt` | number | count | Monotonically increasing reboot counter, NVS-persisted across power cycles. The cloud detects every increment as a reboot event. Covers: power cycles, watchdog resets, crashes, OTA reboots, REBOOT commands. |
| `_reboot_reason` | string | — | Why the device rebooted. Read from the nRF9160 hardware `RESETREAS` register via `hwinfo_get_reset_cause()` on every boot. The register is cleared after reading so each boot gets a fresh value. NVS-persisted so it survives if the device reboots again before publishing. See reason values below. |
| `_modem_temp` | number | °C | Modem die temperature (AT%XTEMP). Normal: 20–60°C. Modem auto powers off above 85°C. Requires `CONFIG_CONEXIO_CLOUD_PSM=y`. |
| `_battery_mv` | number | mV | Battery voltage (AT%XVBAT). Resolution: 4 mV. Requires `CONFIG_CONEXIO_CLOUD_AUTO_BATTERY=y`. |

#### `_reboot_reason` values

The nRF9160 hardware `RESETREAS` register is a bitmask — multiple bits can be
set at once. The SDK picks the single most actionable reason using this
priority order: **watchdog → lockup → brownout → software → pin → por → wake → debug**.

| Value | Hardware cause | What it means | Action |
|-------|---------------|---------------|--------|
| `watchdog` | `RESET_WATCHDOG` | Hardware watchdog timer expired. The main loop stopped kicking the watchdog — stall, deadlock, or an infinite blocking call. | Inspect what code path is blocking. Check `retry_kick_watchdog()` call sites. Review thread stack high-water marks. |
| `lockup` | `RESET_CPU_LOCKUP` | CPU lockup detected after a hard fault (e.g. null pointer dereference, stack overflow, bus fault). The CPU core locked up and the SoC hardware reset it. | Capture a coredump if possible. Review recent code changes near the last-known instruction. Enable `CONFIG_FAULT_DUMP=2` for verbose fault output. |
| `brownout` | `RESET_BROWNOUT` | Supply voltage dropped below the brownout threshold (~1.7 V) while the device was running. | Check battery capacity and discharge curve. Look for high-current events (LTE tx burst) that cause a voltage dip. Review power supply decoupling. |
| `software` | `RESET_SOFTWARE` | `sys_reboot()` was called in firmware — intentional reset. Sources: REBOOT command from dashboard, OTA reboot after FOTA, max retry attempts reached in `retry.c`. | Normal in isolation. Unexpected frequency indicates retry storms or excessive REBOOT commands. Check `_reboot_cnt` trend. |
| `pin` | `RESET_PIN` | External reset pin was asserted — physical button press, or an external supervisor IC (e.g. voltage monitor with RESET output) pulled the pin low. | Normal for user-triggered resets. Unexpected occurrences indicate hardware instability on the reset line or a supervisor IC firing on undervoltage. |
| `por` | `RESET_POR` | Power-on reset — first boot after power was completely removed and re-applied (battery connect, cold start). | Normal. Expected after manufacturing, deployment, or battery swap. First `_reboot_cnt` value after POR is typically 1. |
| `wake` | `RESET_LOW_POWER_WAKE` | Device woke from System OFF (the nRF9160's deepest sleep state). Normal and expected for devices using PSM with very long intervals that enter System OFF between transmissions. | Normal. If unexpected, check whether `sys_reboot(SYS_REBOOT_COLD)` is being called when only a warm wake was intended. |
| `debug` | `RESET_DEBUG` | Reset triggered by a connected debugger or programmer (J-Link, SWD CTRL-AP). | Development/flashing only. Should not appear in production deployments. If it does in the field, investigate physical access or a misbehaving debug port. |
| `unknown` | 0 / ambiguous | `RESETREAS` register was zero (cleared before reading) or contained an unrecognised combination of flags. | Rare. Can happen if the device lost power mid-reset-cycle. Not actionable on its own — correlate with `_reboot_cnt` trend. |
| `unavailable` | n/a | `CONFIG_HWINFO=y` was not set in prj.conf. The SDK selects it automatically — this value should not appear in normal builds. | Add `CONFIG_HWINFO=y` to prj.conf, or confirm `CONFIG_CONEXIO_CLOUD=y` is selecting it. |

#### Connectivity health
Boot-once: `_lte_connect_ms`. Every publish: `_conn_loss`, `_reset_loop`.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_lte_connect_ms` | number | ms | Time from boot (calling `lte_lc_connect_async()`) to first LTE registration. High values indicate SIM, coverage, or modem initialisation problems. Omitted if 0. |
| `_conn_loss` | number | count | Number of times the device dropped LTE registration and had to re-register since last boot. Each drop increments by 1. Always published (0 = no drops). |
| `_reset_loop` | number | 0 or 1 | `1` if the modem detected a reset loop (restricts network attach for 30 minutes). Only published when `1`. Indicates excessive reboots in a short window. |

#### Data usage
Accumulated since boot. Requires `modem_info_connectivity_stats_init()` which the SDK calls automatically.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_tx_kb` | number | kB | Kilobytes transmitted by the modem since boot. Detects runaway payloads or unexpected data usage. |
| `_rx_kb` | number | kB | Kilobytes received by the modem since boot. |

#### Power saving (PSM/eDRX)
Boot-once. Only published when the network has confirmed the parameter. Requires `CONFIG_CONEXIO_CLOUD_PSM=y`.

| Metric | Type | Unit | Description |
|--------|------|------|-------------|
| `_psm_tau_sec` | number | seconds | Actual Periodic TAU timer granted by the network (T3412). May differ from the requested value. `-1` = PSM not granted. |
| `_psm_active_sec` | number | seconds | Actual PSM active window granted (T3324). Time the modem stays awake after waking. `-1` = PSM not granted. |
| `_edrx_ms` | number | ms | Actual eDRX cycle interval granted (ms). `0` = eDRX not active. |
| `_edrx_ptw_ms` | number | ms | Actual eDRX Paging Time Window granted (ms). Affects command delivery latency. |

### Example payload

```json
{
  "deviceId": "351358815179730",
  "timestamp": "2026-06-11T09:15:32.412Z",
  "metrics": {
    "_rssi":           -87,
    "_snr":            39,
    "_lte_mode":       7,
    "_lte_band":       20,
    "_cell_id":        12648450,
    "_tac":            5140,

    // ── First publish after boot only (omitted on all subsequent payloads) ──
    "_operator":       "Telia SE",          // carrier never changes mid-session
    "_modem_fw":       "mfw_nrf9160_1.3.6", // modem FW never changes at runtime
    "_sdk_version":    "2.1.0",             // SDK version never changes at runtime

    // ── Every publish ──────────────────────────────────────────────────────
    "_reboot_cnt":     14,
    "_reboot_reason":  "software",
    "_modem_temp":     32,
    "_battery_mv":     3812,
    "_lte_connect_ms": 4230,
    "_conn_loss":      0,
    "_tx_kb":          42,
    "_rx_kb":          8,
    "_psm_tau_sec":    3600,
    "_psm_active_sec": 10,
    "temperature":     22.5,
    "humidity":        61.0
  }
}
```

---

## Frontend & Backend Integration Guide

This section documents what to add to the frontend and backend when you want
to visualise and alert on these metrics. Work through these when ready.

### Backend — DynamoDB / Lambda

#### New metric-to-field mappings (add to `telemetry/handler.ts` processor)

All metrics are already stored as-is in the telemetry DynamoDB table under
their `_`-prefixed key names. The following additional processing is
recommended for specific metrics:

**`_reboot_cnt` — reboot tracking (already implemented)**
The existing `connectivity/tracker.ts` detects increments and writes reboot
events. No change needed.

**`_reboot_reason` — reboot cause**
```typescript
// Attach the reboot reason to every reboot event written by tracker.ts
// so the Fleet Health Reboot Tracking tab shows cause alongside timestamp.
if (metrics._reboot_cnt > lastKnownCount) {
  await writeRebootEvent(deviceId, {
    count:     metrics._reboot_cnt,
    reason:    metrics._reboot_reason ?? 'unknown',
    timestamp,
  });
}

// Alert on non-intentional reboots
const ALERT_REASONS = ['watchdog', 'lockup', 'brownout'];
if (ALERT_REASONS.includes(metrics._reboot_reason)) {
  await writeAlert(deviceId, 'UNEXPECTED_REBOOT', {
    reason: metrics._reboot_reason,
    count:  metrics._reboot_cnt,
    timestamp,
  });
}
```

**`_conn_loss` — connection drop counter**
```typescript
// In telemetry processor: detect non-zero conn_loss and write a fleet event
if (metrics._conn_loss && metrics._conn_loss > 0) {
  await writeFleetEvent(deviceId, 'CONNECTION_DROP', {
    count: metrics._conn_loss,
    timestamp
  });
}
```

**`_reset_loop` — modem reset loop alert**
```typescript
// Write a high-severity fleet event when reset loop is detected
if (metrics._reset_loop === 1) {
  await writeFleetEvent(deviceId, 'MODEM_RESET_LOOP', { timestamp });
}
```

**`_modem_temp` — thermal alert**
```typescript
// Alert when modem die temperature exceeds warning threshold
const MODEM_TEMP_WARN_C  = 75;  // Warning
const MODEM_TEMP_CRIT_C  = 83;  // Critical (modem shuts down at 85°C)
if (metrics._modem_temp !== undefined) {
  if (metrics._modem_temp >= MODEM_TEMP_CRIT_C) {
    await writeAlert(deviceId, 'MODEM_OVERTEMP_CRITICAL', metrics._modem_temp);
  } else if (metrics._modem_temp >= MODEM_TEMP_WARN_C) {
    await writeAlert(deviceId, 'MODEM_OVERTEMP_WARNING', metrics._modem_temp);
  }
}
```

**`_rssi` — signal quality decoding**
```typescript
// Convert raw RSRP index to dBm for display
function rsrpIndexToDbm(idx: number): number {
  return idx < 0 ? idx - 140 : idx - 141;
}

// Signal quality classification
function classifySignal(rssiIdx: number): 'excellent' | 'good' | 'fair' | 'poor' {
  const dbm = rsrpIndexToDbm(rssiIdx);
  if (dbm >= -80)  return 'excellent';
  if (dbm >= -90)  return 'good';
  if (dbm >= -100) return 'fair';
  return 'poor';
}
```

**`_snr` — SNR index to dB conversion**
```typescript
// SNR index to dB: value - 24
// E.g. index 39 → 15 dB, index 24 → 0 dB
function snrIndexToDb(idx: number): number {
  return idx - 24;
}
```

**`_lte_mode` — human-readable LTE mode**
```typescript
const LTE_MODE_LABELS: Record<number, string> = {
  7: 'LTE-M',
  9: 'NB-IoT',
  0: 'Unknown',
};
```

**`_cell_id` + `_tac` — cell-level location**
These can be forwarded to a geolocation API (e.g. OpenCelliD, HERE, Google
Maps Geolocation API) to derive approximate device location without GPS:
```typescript
// Forward to geolocation service
async function resolveLocation(mcc, mnc, tac, cellId) {
  // POST to /v1/cell/lookup with { mcc, mnc, lac: tac, cellId }
  // Returns { lat, lng, accuracy }
}
```
Store `_cell_id` and `_tac` in a separate `device_location` table alongside
`mcc`/`mnc` from operator lookup for efficient geolocation queries.

**`_tx_kb` + `_rx_kb` — session data budget tracking**
```typescript
// Track cumulative data usage per device per month for billing analysis
// Store max(_tx_kb) and max(_rx_kb) per reboot session
// Delta between consecutive max values = data used that session
```

#### New DynamoDB attributes to index

Add GSIs or filter expressions for:

| Attribute | Type | Purpose |
|-----------|------|---------|
| `_lte_mode` | Number | Query LTE-M vs NB-IoT device breakdown |
| `_lte_band` | Number | Query devices by frequency band |
| `_operator` | String | Query devices by carrier |
| `_modem_fw` | String | Fleet firmware version distribution |
| `_cell_id` | Number | Group devices by cell for location |

### Frontend — Dashboard Widgets

#### Signal Quality page — add to existing RSSI sparkline

```typescript
// New derived field in telemetry data layer
rssiDbm: rsrpIndexToDbm(d._rssi),
snrDb:   snrIndexToDb(d._snr),
```

Recommended widget additions:

| Widget | Metrics | Type |
|--------|---------|------|
| Signal Quality | `_rssi` (converted to dBm) | Sparkline with thresholds: -80=green, -100=amber, -120=red |
| SNR | `_snr` (converted to dB) | Sparkline |
| LTE Mode | `_lte_mode` | String badge: "LTE-M" / "NB-IoT" |
| Active Band | `_lte_band` | Single value |
| Operator | `_operator` | String badge |

#### Fleet Health page — new panels

| Panel | Metrics | Description |
|-------|---------|-------------|
| Connection Drops | `_conn_loss` | Time-series count of drops across fleet |
| Boot-to-Connected Time | `_lte_connect_ms` | Histogram of connect times |
| Reset Loop Events | `_reset_loop` | Count of occurrences with timestamps |
| Modem Temperature | `_modem_temp` | Sparkline with 75°C/83°C threshold lines |
| Data Usage | `_tx_kb`, `_rx_kb` | Stacked bar per session |
| Carrier Distribution | `_operator` | Pie chart across fleet |
| LTE Mode Distribution | `_lte_mode` | Pie chart: LTE-M vs NB-IoT |
| Band Distribution | `_lte_band` | Bar chart by band number |
| Modem FW Distribution | `_modem_fw` | Bar chart for fleet upgrade tracking |

#### Device Detail page — add to the overview card

```typescript
interface DeviceOverviewCard {
  // Existing
  deviceId:    string;
  lastSeen:    string;
  rssi:        number;   // _rssi (raw index)
  battery:     number;   // _battery_mv

  // New
  rssiDbm:     number;   // rsrpIndexToDbm(_rssi)
  snrDb:       number;   // snrIndexToDb(_snr) — shown as "15 dB"
  lteMode:     string;   // "LTE-M" | "NB-IoT"
  lteBand:     number;   // e.g. 20
  operator:    string;   // e.g. "Telia SE"
  cellId:      number;   // for location lookup
  tac:         number;   // for location lookup
  modemFw:     string;   // e.g. "mfw_nrf9160_1.3.6"
  modemTemp:   number;   // in °C — colour red if > 75
  txKb:        number;   // session data usage
  rxKb:        number;
  connLoss:    number;   // drops since last boot
  connectMs:   number;   // boot-to-connected time
  psm: {
    tauSec:    number;   // _psm_tau_sec
    activeSec: number;   // _psm_active_sec
  };
}
```

#### Threshold / alerting rules to configure

| Condition | Threshold | Severity |
|-----------|-----------|----------|
| `rsrpIndexToDbm(_rssi)` < -120 | Signal very poor | Warning |
| `rsrpIndexToDbm(_rssi)` < -130 | Signal critical | Critical |
| `_snr` < 10 (= -14 dB) | Low SNR | Warning |
| `_conn_loss` > 3 per session | Unstable connection | Warning |
| `_lte_connect_ms` > 30000 (30s) | Slow to connect | Warning |
| `_reset_loop` = 1 | Modem reset loop | Critical |
| `_reboot_reason` = `"watchdog"` | WDT expired — stall/deadlock | Critical |
| `_reboot_reason` = `"lockup"` | CPU hard fault / crash | Critical |
| `_reboot_reason` = `"brownout"` | Supply voltage collapse | Critical |
| `_reboot_reason` = `"software"` frequent (> 5/day) | Retry storm or crash loop | Warning |
| `_modem_temp` > 75 | High modem temp | Warning |
| `_modem_temp` > 83 | Critical modem temp | Critical |
| `_tx_kb` > 500 per session | Abnormal data usage | Warning |
