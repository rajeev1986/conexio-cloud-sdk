# Cellular Location — AT%NCELLMEAS Cell Positioning

Demonstrates gathering neighboring cell measurements via `AT%NCELLMEAS` and
sending them to Conexio Cloud for server-side positioning.

No positioning solver runs on the device. The firmware gathers raw cell data;
the Conexio Cloud Lambda calls the HERE Positioning API and writes the resolved
coordinates to AWS Location Tracker. The result appears as a pin on the
Conexio Console Fleet Map.

---

## What this sample does

1. Initialises the cellular location module after LTE connects
2. Triggers `AT%NCELLMEAS` every `CONFIG_CELL_LOCATION_INTERVAL_SEC` seconds
3. Collects serving cell + up to 17 neighbor cells
4. Queues `_loc_*` metrics — they are published to the location topic
5. Cloud Lambda resolves the cell data to `{lat, lng, accuracy}` via HERE API

---

## Location metrics published

| Metric | Description | Example |
|--------|-------------|---------|
| `_loc_mcc` | Mobile Country Code | `311` |
| `_loc_mnc` | Mobile Network Code | `480` |
| `_loc_cell_id` | E-UTRAN Cell ID | `129061889` |
| `_loc_tac` | Tracking Area Code | `52228` |
| `_loc_earfcn` | Frequency channel | `5230` |
| `_loc_rsrp` | Signal strength (dBm) | `-85` |
| `_loc_timing_adv` | Distance proxy | `16` |
| `_loc_neighbors` | Neighbor cell array | `[{"earfcn":5110,"pci":42}]` |

---

## prj.conf settings

| Option | Value | Description |
|--------|-------|-------------|
| `CONFIG_CELL_LOCATION` | `y` | Enable AT%NCELLMEAS module |
| `CONFIG_CELL_LOCATION_INTERVAL_SEC` | `3600` | Fix every hour |
| `CONFIG_CELL_LOCATION_PUBLISH_ON_FIX` | `y` | Publish immediately after fix |
| `CONFIG_CELL_LOCATION_SEARCH_TYPE` | `1` | EXTENDED_LIGHT — 3–8 neighbors |

**Search types:**
- `0` = DEFAULT — fast (1–3s), 0–2 neighbors, ±1–5 km accuracy
- `1` = EXTENDED_LIGHT — moderate (3–8s), 3–8 neighbors, ±300m–1km *(default)*
- `2` = EXTENDED — thorough (5–15s), up to 17 neighbors, best accuracy

---

## Build and flash

```bash
west build -b conexio_stratus_pro/nrf9151/ns --pristine
west flash
```

---

## Expected serial output

```
[00:00:00.257] <inf> app: === Conexio Cellular Location Sample ===
[00:00:00.257] <inf> app: Fix interval: 3600s | Publish on fix: yes
[00:00:19.533] <inf> mqtt_transport: MQTT connected
[00:00:19.787] <inf> app: Connected — 355025934980275 | App v1.0.0
[00:00:21.900] <inf> app: Cell location active — first fix in 3600s
...
[01:00:21.900] <inf> cell_location: AT%NCELLMEAS result: serving + 5 neighbors
[01:00:22.100] <inf> cell_location: Location metrics queued — publishing
```

---

## Cloud location payload

```json
{
  "dev_id": "355025934980275",
  "ts": "2026-09-20T15:00:22.000Z",
  "metrics": {
    "_loc_mcc": 311,
    "_loc_mnc": 480,
    "_loc_cell_id": 129061889,
    "_loc_tac": 52228,
    "_loc_earfcn": 5230,
    "_loc_rsrp": -85,
    "_loc_timing_adv": 16,
    "_loc_neighbors": "[{\"earfcn\":5110,\"pci\":42,\"rsrp\":-90}]"
  }
}
```

The Conexio Cloud Lambda resolves this to `{lat, lng, accuracy}` and writes
it to AWS Location Tracker. View the result in Console → Fleet Map.

---

## Production guidance

| Use case | Recommended interval |
|----------|---------------------|
| Development / testing | `60s` |
| Active field tracking | `14400s` (4 hours) |
| Asset management | `28800s` (8 hours) |

Reducing fixes to 3×/day keeps HERE API costs well within free tier for
fleets up to ~100 devices.
