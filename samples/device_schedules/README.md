# Device Schedules — Timed Command Delivery

Demonstrates the Conexio Device Schedules feature: sending a START command
at a scheduled time and a STOP command at another time, with the firmware
executing the STOP autonomously even if the device loses connectivity.

The SDK stores the schedule in NVS flash. If the device reboots or loses
connectivity between start and stop, it still fires the STOP command when
it next boots or reconnects — the schedule is never silently dropped.

---

## What this sample does

- Registers `LED_ON` (schedule start) and `LED_OFF` (schedule stop)
- Registers a schedule lifecycle callback for STARTED / STOPPED / EXPIRED events
- The firmware watchdog timer ensures `LED_OFF` fires at `stopAt` even without
  cloud connectivity

---

## How the watchdog works

```
Cloud delivers LED_ON + stopAt
  │
  ├─ on_led_on(): LED turns on
  ├─ SDK stores {stop_command="LED_OFF", stopAt=...} in NVS
  ├─ SDK arms k_timer for (stopAt - now)
  │
  │  [device may lose connectivity or reboot here]
  │
  ├─ k_timer fires → on_led_off() called autonomously
  └─ SDK clears NVS schedule
```

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CONEXIO_CLOUD_INTERVAL_SEC` | `60` | Telemetry interval |
| `CONFIG_GPIO` | `y` | LED GPIO |
| `CONFIG_NVS` | `y` | Required for schedule NVS persistence |

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash
```

---

## How to test

1. Flash and wait for the device to connect
2. Open Conexio Console → your device → Schedules tab
3. Create a schedule:
   - Start command: `LED_ON`
   - Stop command: `LED_OFF`
   - Start time: ~2 minutes from now
   - Stop time: ~7 minutes from now
4. Watch serial output at start time:
   ```
   LED_ON — LED is ON
   Schedule STARTED — 'LED_OFF' will fire at stopAt (watchdog armed)
   ```
5. Disconnect USB power between start and stop — the LED is off the device
6. Reconnect at the stop time: `LED_OFF` fires immediately on reconnect

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Device Schedules Sample ===
[00:00:00.257] <inf> app: LED ready (pin 25)
[00:00:19.533] <inf> mqtt_transport: MQTT connected
[00:00:19.787] <inf> app: Connected — 355025934980275 | App v1.0.0
...
[00:02:00.000] <inf> app: LED_ON — LED is ON
[00:02:00.010] <inf> app: Schedule STARTED — 'LED_OFF' will fire at stopAt
...
[00:07:00.000] <inf> app: LED_OFF — LED is OFF
[00:07:00.010] <inf> app: Schedule STOPPED — 'LED_OFF' fired autonomously
```

**Boot during active window:**
```
[00:00:00.257] <wrn> app: Schedule EXPIRED on boot — 'LED_OFF' executed immediately
```
