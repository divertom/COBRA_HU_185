# TPMS BLE architecture and flow charts

COBRA_HU_185 uses a **single ESP32 BLE controller** shared by:

- **SmartRemote** — persistent GATT client (HID + battery) in `main/Wireless/Wireless.c`
- **tsTPMS modules** — one GATT client at a time in `main/TPMS/tesla_tpms_ble.c`, orchestrated by `main/TPMS/tpms_manager.c`
- **Background scan** — discovers advertisers and updates wheel-sensor slots from manufacturer data

Four simultaneous tsTPMS GATT connections plus SmartRemote is **not** possible on one radio. After portal discovery, firmware **round-robins** through known tsTPMS MACs (~15 s per slot, ~2 s gap).

## Timing constants

| Constant | Value | Purpose |
|----------|-------|---------|
| `TPMS_ROTATE_SLOT_MS` | 15 s | Max time connected per tsTPMS before next MAC |
| `TPMS_ROTATE_GAP_MS` | 2 s | BLE scan window between GATT slots |
| `TPMS_ROTATE_CONNECT_TIMEOUT_MS` | 10 s | Give up connect if not connected |

Primary GATT MAC (portal **Set live**) is polled **first** in the rotation queue when set.

---

## High-level radio sharing

```mermaid
flowchart TB
    subgraph ESP32["ESP32 BLE controller"]
        GAP["GAP: scan + connect"]
        GATT_R["GATT client: SmartRemote"]
        GATT_T["GATT client: tsTPMS (one target)"]
    end

    SR["SmartRemote HID"]
    T1["tsTPMS #1"]
    T2["tsTPMS #2"]
    T3["tsTPMS #3"]
    T4["tsTPMS #4"]
    ADV["Wheel sensors (adv only)"]

    GAP --> ADV
    GAP --> T1
    GAP --> T2
    GAP --> T3
    GAP --> T4
    GATT_R <-->|"stays connected"| SR
    GATT_T <-->|"one at a time"| T1
    GATT_T -.->|"rotation"| T2
    GATT_T -.-> T3
    GATT_T -.-> T4
```

---

## Portal discovery scan (Start / Stop Scan)

**Start Scan:** advertising only — no GATT. New MACs saved to SPIFFS `/config/tpms_sensors.json`.

**Stop Scan:** stops discovery scan, starts **GATT rotation** if any `is_gateway` (tsTPMS) entries exist.

```mermaid
stateDiagram-v2
    [*] --> Idle
    Idle --> Scanning: POST /api/tpms/scan active=true
    Scanning --> Scanning: GAP ADV reports\nclassify + SPIFFS save\nno GATT
    Scanning --> Rotating: Stop scan\nactive=false
    Rotating --> Scanning: Start scan again\nrotation stopped
    Idle --> Rotating: Boot with saved\ntsTPMS modules
```

```mermaid
sequenceDiagram
    participant UI as Config portal
    participant MGR as tpms_manager
    participant BLE as Wireless / GAP
    participant ADV as tpms_ble_adv

    UI->>MGR: Start Scan
    MGR->>MGR: rotation stop, tesla_tpms_stop
    MGR->>BLE: RestartBleScanForTpms
  loop While scan_active
        BLE-->>MGR: ESP_GAP_BLE_SCAN_RESULT_EVT
        MGR->>ADV: classify payload
        ADV-->>MGR: gateway / wheel
        MGR->>MGR: add slot, save JSON
        Note over MGR: Log "GATT deferred until scan stops"
    end
    UI->>MGR: Stop Scan
    MGR->>MGR: tpms_manager_start_rotation
```

---

## GATT rotation state machine

Worker task: `tpms_rot` (`tpms_rotation_task` in `tpms_manager.c`).

```mermaid
stateDiagram-v2
    [*] --> GAP_PHASE: start_rotation
    GAP_PHASE --> SLOT: gap elapsed (2s)\nbegin_slot
    SLOT --> SLOT: connecting / connected\nwait for pressure
    SLOT --> GAP_PHASE: stable/active telemetry\nOR timeout OR disconnect
    GAP_PHASE --> GAP_PHASE: advance cursor\n(mod count)
    note right of SLOT
        tesla_tpms_set_target(MAC)
        tesla_tpms_request_connect
        is_gatt_active = true in API
    end note
```

```mermaid
flowchart TD
    A[Rotation task wake] --> B{scan_active?}
    B -->|yes| Z[skip]
    B -->|no| C{SmartRemote CONNECTING?}
    C -->|yes| Z
    C -->|no| D{phase == GAP?}
    D -->|yes, time ok| E[begin_slot: connect to MAC n]
    D -->|no| F{phase == SLOT?}
    F --> G{got_read?}
    G -->|yes| H[end_slot: stop GATT, scan 2s]
    G -->|no| I{connected && slot >= 15s?}
    I -->|yes| H
    I -->|no| J{connect timeout 10s?}
    J -->|yes| H
    H --> D
```

Queue order: tire positions LF → RF → LR → RR; **primary_gatt_mac** sorts first when configured.

---

## Telemetry path

```mermaid
sequenceDiagram
    participant T as tsTPMS module
    participant GATT as tesla_tpms_ble
    participant MGR as tpms_manager
    participant ROT as tpms_rotation_task

    T->>GATT: GATT indicate (pressure)
    GATT->>GATT: tpms_parser
    GATT->>MGR: on_gateway_telemetry
    MGR->>MGR: update slot, SPIFFS units
    MGR->>ROT: s_rotate_got_read = true, notify
    ROT->>ROT: end_slot, next MAC after gap
```

---

## SmartRemote coexistence

| SmartRemote state | Portal scan | GATT rotation |
|-------------------|-------------|---------------|
| CONNECTED | Allowed (scan only) | Allowed (one tsTPMS GATT) |
| CONNECTING | Scan OK | **Deferred** until connected |
| Disconnected | Scan OK | Rotation continues; remote reconnect uses scan |

Policy: block tsTPMS GATT only while remote is **connecting**, not while **connected** (`tpms_gateway_link_blocked_internal`).

```mermaid
flowchart LR
    SR_CONN[SmartRemote CONNECTED]
    SR_CON[SmartRemote CONNECTING]
    SCAN[TPMS scan_active]
    ROT[GATT rotation]

    SR_CON -->|blocks| ROT
    SR_CONN -->|allows| ROT
    SCAN -->|blocks| ROT
    SCAN -->|allows| ADV_only
```

---

## Disconnect handling

```mermaid
flowchart TD
    DISC[ESP_GATTC_DISCONNECT_EVT] --> SCAN{scan_active?}
    SCAN -->|yes| END[restart scan only]
    SCAN -->|no| ROT{rotation_active\nand in SLOT?}
    ROT -->|yes| SLOT[end_slot early]
    ROT -->|no| R2{rotation_active?}
    R2 -->|yes| WAKE[notify rotation task]
    R2 -->|no| GW[request_gateway_link\nstarts rotation via worker]
```

---

## Config portal API

`GET /api/tpms` includes:

- `scan_active` — discovery mode
- `rotation_active` — round-robin running
- per sensor: `is_gatt_active` — MAC currently in GATT slot

UI shows **Reading** on the active row and status *Rotating GATT reads across sensors…* when idle after scan.

---

## Key source files

| File | Role |
|------|------|
| `main/TPMS/tpms_manager.c` | Scan, persistence, rotation task, snapshots |
| `main/TPMS/tesla_tpms_ble.c` | Single GATT client to tsTPMS |
| `main/TPMS/tpms_ble_adv.c` | Gateway vs wheel advertisement parsing |
| `main/Wireless/Wireless.c` | SmartRemote GATT, shared GAP scan |
| `main/Config_Portal/Config_Portal.c` | REST `/api/tpms` |
| `main/Config_Portal/web/service.js` | TPMS panel UI |

---

## Expected log patterns (test)

1. **Start Scan** — `Discovered gateway tsTPMS … (GATT deferred until scan stops)`; no `[TPMS] GATT client started`.
2. **Stop Scan** — `GATT rotation started (N tsTPMS modules, 15000 ms per slot)`; cycling `GATT rotation slot k/N` and `STABLE pressure`.
3. **SmartRemote connected** — rotation still advances; no perpetual `Gateway GATT deferred` while remote is only connected.
