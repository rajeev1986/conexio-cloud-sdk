# Offline Buffer — Local Telemetry Buffering

Demonstrates how the Conexio Cloud SDK automatically buffers telemetry
in NVS flash when the device loses connectivity, then replays it to the
cloud in chronological order on reconnect.

No application code changes are needed to enable offline buffering —
just set `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER=y` in `prj.conf`.

---

## What this sample does

- Enables the NVS ring buffer with 50 entries
- Streams temperature readings every 30 seconds
- When offline: payloads accumulate in flash, log shows buffer count
- On reconnect: SDK replays 5 buffered entries per session
- Payloads older than 24 hours are discarded during replay
- Optional `on_buffer_full()` callback logs when the ring wraps

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER` | `y` | Enable flash buffering |
| `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_SIZE` | `50` | Max entries in ring buffer |
| `CONFIG_CONEXIO_CLOUD_OFFLINE_REPLAY_BATCH` | `5` | Entries replayed per reconnect |
| `CONFIG_CONEXIO_CLOUD_OFFLINE_BUFFER_TTL_SEC` | `86400` | Discard after 24 hours |

**Coverage estimate:**
`50 entries × 30s interval = ~25 minutes of offline coverage`

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash
```

---

## How to test

1. Flash and wait for the device to connect and publish once
2. Simulate a connectivity loss — block LTE signal, or disconnect antenna
3. Watch buffer entries accumulate in the serial log:
   ```
   Disconnected — telemetry will buffer to flash
   ```
4. Restore connectivity — SDK replays buffered payloads:
   ```
   Offline buffer: 8/50 payloads pending
   Published (buffer: 7 pending)
   Published (buffer: 6 pending)
   ...
   ```
5. The dashboard timeline shows the buffered readings at their original timestamps

---

## Expected serial output

**While connected:**
```
[00:00:19.533] <inf> mqtt_transport: MQTT connected
[00:00:21.900] <inf> app: Published (buffer: 0 pending)
[00:00:51.900] <inf> app: Published (buffer: 0 pending)
```

**Connectivity loss:**
```
[00:01:05.000] <wrn> app: Disconnected — telemetry will buffer to flash
[00:01:35.000] <inf> offline_buf: Buffered 1/50 payloads
[00:02:05.000] <inf> offline_buf: Buffered 2/50 payloads
```

**Reconnect and replay:**
```
[00:10:00.000] <inf> mqtt_transport: MQTT connected
[00:10:00.500] <inf> conexio_cloud: Offline buffer: 16/50 payloads pending
[00:10:02.000] <inf> app: Published (buffer: 15 pending)
[00:10:03.000] <inf> app: Published (buffer: 14 pending)
```

---

## Cloud payload (replayed entry)

Replayed payloads carry their **original timestamp**, so the dashboard
timeline accurately reconstructs the gap:

```json
{
  "dev_id": "355025934980275",
  "ts": "2026-09-20T14:05:35.000Z",
  "metrics": {
    "temperature": 24.3,
    "_rssi": 55
  }
}
```
