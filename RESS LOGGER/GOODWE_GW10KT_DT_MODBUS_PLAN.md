# GoodWe GW10KT-DT Modbus Data Plan

## Document status

| Item | Value |
|---|---|
| Project | RESS Logger |
| Inverter | GoodWe GW10KT-DT, SDT G2 series |
| Document purpose | Shared firmware/IoT data-contract specification |
| Plan status | Implemented and build-verified on STM32 and ESP32 |
| Last updated | 2026-11-08 |
| Important rule | Keep this file synchronized with firmware payload/CSV changes |

This document records the agreed plan before implementation so the STM32,
ESP32, MQTT, backend, and IoT teams use the same register definitions and
field names.

## System scope

The GW10KT-DT is a three-phase, grid-tied inverter with two MPPT trackers.
This integration uses Modbus RTU to collect inverter data on the STM32, saves
scheduled snapshots to CSV, transfers the snapshot to the ESP32, and publishes
it to MQTT.

```text
GoodWe GW10KT-DT
       |
       | Modbus RTU
       v
STM32 logger -----> SD card CSV
       |
       | framed UART telemetry
       v
ESP32 gateway ----> MQTT broker ----> IoT/backend platform
```

Battery, backup-output, and diesel-generator measurements are outside the
scope because the GW10KT-DT is a grid-tied inverter without those ports.

## Communication configuration

| Setting | Value |
|---|---:|
| Protocol | Modbus RTU |
| Function code | `0x03` Read Holding Registers |
| Slave address | `247` (`0xF7`) |
| Baud rate | `9600` |
| Data bits | `8` |
| Parity | None |
| Stop bits | `2` |
| Normal polling target | 5 seconds |
| Offline retry target | 15 seconds |
| Register notation | Decimal wire addresses from the GoodWe document |
| U32/S32 word order | High 16-bit word first |

## Planned Modbus transactions

The implementation uses bounded, model-appropriate blocks. It does not
bulk-read `768-896`, because that range contains MT/SMT-only and reserved
registers that may cause Modbus exceptions on an SDT G2 inverter.

| Transaction | Start | Count | Schedule | Purpose |
|---|---:|---:|---|---|
| Device serial | 512 | 8 | Once after connection | 16-byte ASCII serial number |
| Device type | 528 | 5 | Once after connection | Device/model ASCII string |
| Main SDT G2 telemetry | 544 | 23 | Every poll | Registers 544 through 566 |
| Power factor | 886 | 1 | Every poll | SDT G2 power factor |
| Extended diagnostics | 893 | 4 | Every poll | Reactive power, insulation, leakage |
| Optional line voltages | 30115 | 3 | Probe, then poll if supported | AB, BC, CA voltages |
| Optional input power | 30137 | 2 | Probe, then poll if supported | Total input/PV power |

### Optional-register behavior

Registers `30115-30117` and `30137-30138` require compatible SDT G2 firmware.
The proposed behavior is:

1. Probe each optional block once after normal communication is established.
2. If it succeeds, include it in normal polling.
3. If it returns Modbus exception `0x02` (illegal address), mark it unsupported.
4. Do not retry an unsupported optional block on every polling cycle.
5. Continue collecting all standard fields when optional blocks are absent.

## Register and field map

### Device identity and operating state

| Proposed key | Register(s) | Raw format | Conversion | Unit/values |
|---|---:|---|---|---|
| `device_serial` | 512-519 | STR, 8 registers | Decode 16 ASCII bytes; trim padding | Text |
| `device_type` | 528-532 | STR, 5 registers | Decode ASCII bytes; trim padding | Text |
| `error_code` | 544-545 | U32 | Preserve as unsigned value and hexadecimal bitmap | Bit field |
| `operating_hours_total` | 548-549 | U32 | Direct | h |
| `working_status` | 564 | U16 | Map with the table below | Text |
| `grid_connection_status` | Derived from 564 | Derived | `on_grid` only when work mode is 1 | Text |
| `inverter_temperature` | 565 | U16 | Raw / 10 | degC |

#### Working-status mapping

| Raw value | Published value | Meaning |
|---:|---|---|
| 0 | `standby` | Waiting or self-test |
| 1 | `on_grid` | Normal power generation |
| 2 | `fault` | Inverter fault state |
| Other | `unknown` | Undocumented value |

The SEMS label "Chamber Temperature" is represented as
`inverter_temperature`, because register 565 is specifically the inverter's
internal temperature.

### DC and MPPT measurements

| Proposed key | Register/calculation | Raw format | Conversion | Unit |
|---|---:|---|---|---:|
| `mppt1_voltage` | 550 | U16 | Raw / 10 | V |
| `mppt2_voltage` | 551 | U16 | Raw / 10 | V |
| `mppt1_current` | 552 | U16 | Raw / 10 | A |
| `mppt2_current` | 553 | U16 | Raw / 10 | A |
| `mppt1_power` | Derived | Float | `mppt1_voltage * mppt1_current` | W |
| `mppt2_power` | Derived | Float | `mppt2_voltage * mppt2_current` | W |
| `pv_power_total` | 30137-30138 or derived | S32 or float | Prefer supported register; otherwise MPPT1 + MPPT2 | W |
| `insulation_resistance` | 895 | U16 | Direct | kOhm |
| `leakage_current` | 896 | U16 | Direct | mA |

Generic `pv_voltage` and `pv_current` fields are intentionally not proposed.
Two independent MPPT inputs cannot be represented accurately by a single
voltage/current pair.

For `pv_power_total`, register 30137 is documented as signed S32 with scale
factor 1000 and unit kW. The normalized MQTT/CSV value should be watts. The
implementation must validate the scale against real ModbusPoll samples before
finalizing this conversion.

### AC measurements

| Proposed key | Register/calculation | Raw format | Conversion | Unit |
|---|---:|---|---|---:|
| `active_power` | 563 | U16 | Direct | W |
| `reactive_power` | 893-894 | S32 | Raw / 1000 kVar; normalize to var | var |
| `power_factor` | 886 | U16 | Raw / 1000 | Ratio |
| `phase_a_voltage` | 554 | U16 | Raw / 10 | V |
| `phase_b_voltage` | 555 | U16 | Raw / 10 | V |
| `phase_c_voltage` | 556 | U16 | Raw / 10 | V |
| `phase_a_current` | 557 | U16 | Raw / 10 | A |
| `phase_b_current` | 558 | U16 | Raw / 10 | A |
| `phase_c_current` | 559 | U16 | Raw / 10 | A |
| `phase_a_frequency` | 560 | U16 | Raw / 100 | Hz |
| `phase_b_frequency` | 561 | U16 | Raw / 100 | Hz |
| `phase_c_frequency` | 562 | U16 | Raw / 100 | Hz |
| `grid_frequency` | Derived | Float | Average of valid phase frequencies | Hz |
| `line_ab_voltage` | 30115 | U16 | Raw / 10, if supported | V |
| `line_bc_voltage` | 30116 | U16 | Raw / 10, if supported | V |
| `line_ca_voltage` | 30117 | U16 | Raw / 10, if supported | V |

Phase A/B/C active power is not proposed because the supplied SDT G2 register
map does not expose separate per-phase active-power registers.

### Energy statistics

| Proposed key | Register(s) | Raw format | Conversion | Unit |
|---|---:|---|---|---:|
| `energy_total` | 546-547 | U32 | Raw / 10 | kWh |
| `energy_today` | 566 | U16 | Raw / 10 | kWh |

## Parameters that will not be collected

| SEMS/screenshot parameter | Decision and reason |
|---|---|
| Station name | Cloud/site metadata, not an inverter Modbus register |
| Station address | Cloud/site metadata, not an inverter Modbus register |
| Station type | May be configured as static `grid_tied`; not a measurement |
| Phase A/B/C active power | No matching SDT G2 read-only registers in the supplied protocol |
| Backup-side power/voltage/current | GW10KT-DT has no backup output |
| Standby voltage | No matching read-only register in the supplied protocol |
| Battery power, SOC, voltage, current, temperature, limits | GW10KT-DT has no battery ports |
| Charged/discharged energy | Not applicable to this inverter |
| Diesel generator power/energy | Not applicable to this inverter |
| Single-phase voltage/current | GW10KT-DT is a three-phase inverter |
| AB/AC/BC line current | No separate line-current registers; phase currents are available |
| Off-grid operation | Grid-tied inverter; publish connection status derived from work mode |

## Proposed normalized snapshot

The STM32 keeps validity per value so a failed or unsupported read produces
CSV `NA` and MQTT `null` without invalidating the rest of the snapshot.

```text
identity:
  device_serial, device_type

status:
  error_code, operating_hours_total, working_status,
  grid_connection_status, inverter_temperature

dc:
  mppt1_voltage, mppt1_current, mppt1_power,
  mppt2_voltage, mppt2_current, mppt2_power,
  pv_power_total, insulation_resistance, leakage_current

ac:
  active_power, reactive_power, power_factor,
  phase_a_voltage, phase_b_voltage, phase_c_voltage,
  phase_a_current, phase_b_current, phase_c_current,
  phase_a_frequency, phase_b_frequency, phase_c_frequency,
  grid_frequency,
  line_ab_voltage, line_bc_voltage, line_ca_voltage

energy:
  energy_total, energy_today
```

## Implemented CSV contract

CSV values should use engineering units, not raw register values. Unsupported
or invalid values should be written as `NA`.

```csv
timestamp,epoch,seq,device_serial,device_type,error_code,operating_hours_total,working_status,grid_connection_status,inverter_temperature,mppt1_voltage,mppt1_current,mppt1_power,mppt2_voltage,mppt2_current,mppt2_power,pv_power_total,insulation_resistance,leakage_current,active_power,reactive_power,power_factor,phase_a_voltage,phase_b_voltage,phase_c_voltage,phase_a_current,phase_b_current,phase_c_current,phase_a_frequency,phase_b_frequency,phase_c_frequency,grid_frequency,line_ab_voltage,line_bc_voltage,line_ca_voltage,energy_total,energy_today,gateway_rssi,gateway_uptime,gateway_fw_version,r1,r2,r3,r4,sent
```

Daily files are stored as `0:/GWLOG2/YYYYMMDD.CSV`. The `epoch` is the
original scheduled UTC timestamp and is retained during replay. `sent` starts
at `0` and becomes `1` only after the ESP32 completes the QoS 1 publish.

The final column order must be treated as a versioned contract. Existing CSV
files must not receive rows with a different schema. A new schema should use a
new directory, filename convention, or explicit schema-version column.

## Implemented MQTT contract

The IoT/backend team approved compact batch-item names in contract version 2.
Each item uses `d` (device ID), `k` (parameter key), and `v` (value). The
GoodWe device ID is `inv_gw`; gateway health continues to use `gw`.

Default topic pattern:

```text
ress/<boardId>/telemetry
```

Every payload contains all 34 inverter parameters plus all three gateway
health parameters. A missing, failed, or unsupported value remains present
with `"v": null`; it is never silently omitted and never replaced by zero.
The contract version is `2`.

Payload shape:

```json
{
  "v": 2,
  "boardId": "RB261107RFT001",
  "ts": 1794096000,
  "seq": 26993700,
  "batch": [
    {"d": "inv_gw", "k": "device_serial", "v": "GW10KTDT261107001"},
    {"d": "inv_gw", "k": "device_type", "v": "GW10KT-DT"},
    {"d": "inv_gw", "k": "error_code", "v": 0},
    {"d": "inv_gw", "k": "operating_hours_total", "v": 12580},
    {"d": "inv_gw", "k": "working_status", "v": "on_grid"},
    {"d": "inv_gw", "k": "grid_connection_status", "v": "connected"},
    {"d": "inv_gw", "k": "inverter_temperature", "v": 42.1},
    {"d": "inv_gw", "k": "mppt1_voltage", "v": 512.3},
    {"d": "inv_gw", "k": "mppt1_current", "v": 8.4},
    {"d": "inv_gw", "k": "mppt1_power", "v": 4303.3},
    {"d": "inv_gw", "k": "mppt2_voltage", "v": 509.8},
    {"d": "inv_gw", "k": "mppt2_current", "v": 7.9},
    {"d": "inv_gw", "k": "mppt2_power", "v": 4027.4},
    {"d": "inv_gw", "k": "pv_power_total", "v": 8330.7},
    {"d": "inv_gw", "k": "insulation_resistance", "v": 15.6},
    {"d": "inv_gw", "k": "leakage_current", "v": 12.5},
    {"d": "inv_gw", "k": "active_power", "v": 8010},
    {"d": "inv_gw", "k": "reactive_power", "v": 810},
    {"d": "inv_gw", "k": "power_factor", "v": 0.995},
    {"d": "inv_gw", "k": "phase_a_voltage", "v": 230.2},
    {"d": "inv_gw", "k": "phase_b_voltage", "v": 229.8},
    {"d": "inv_gw", "k": "phase_c_voltage", "v": 231.1},
    {"d": "inv_gw", "k": "phase_a_current", "v": 11.7},
    {"d": "inv_gw", "k": "phase_b_current", "v": 11.6},
    {"d": "inv_gw", "k": "phase_c_current", "v": 11.8},
    {"d": "inv_gw", "k": "phase_a_frequency", "v": 50.01},
    {"d": "inv_gw", "k": "phase_b_frequency", "v": 50.02},
    {"d": "inv_gw", "k": "phase_c_frequency", "v": 50.01},
    {"d": "inv_gw", "k": "grid_frequency", "v": 50.01},
    {"d": "inv_gw", "k": "line_ab_voltage", "v": 398.4},
    {"d": "inv_gw", "k": "line_bc_voltage", "v": 399.1},
    {"d": "inv_gw", "k": "line_ca_voltage", "v": null},
    {"d": "inv_gw", "k": "energy_total", "v": 12345.6},
    {"d": "inv_gw", "k": "energy_today", "v": 31.2},
    {"d": "gw", "k": "rssi", "v": -58},
    {"d": "gw", "k": "uptime", "v": 86400},
    {"d": "gw", "k": "fw_version", "v": "0.1.0"}
  ]
}
```

### MQTT rules for the IoT team

- MQTT values use normalized engineering units.
- Every documented key is included in every batch.
- Unsupported or invalid fields use JSON `null`, not zero and not omission.
- `error_code` is an unsigned JSON number.
- `seq` matches the CSV row and durable queue record.
- Telemetry should use QoS 1 and `retain=false`.
- Device identity is included in every telemetry batch.

### ESP32 serialization memory

The MQTT client and JSON output capacity are both 8192 bytes. The JSON output
buffer is file-scope static storage, not a local `loopTask` variable. This is
required because an 8192-byte local array exhausted the Arduino loop task
stack and triggered `vApplicationStackOverflowHook`. Before serialization,
`measureJson()` verifies that the complete payload fits. The firmware also
prints the loop-task stack high-water mark immediately before publishing.

## Persistent offline store-and-forward

Each CSV row with `sent=0` has a matching durable file under
`0:/GWPEND/<SEQ-HEX>.BUF`. This queue survives STM32 and ESP32 resets.

1. The STM32 writes and synchronizes the CSV row and queue record before it
   sends the snapshot to the ESP32.
2. During an internet outage, scheduled acquisition and SD logging continue.
3. When MQTT reconnects, the ESP32 requests the oldest queue record first.
4. The original `epoch`, `seq`, measurement values, RSSI, uptime, and firmware
   version are replayed; current values are not substituted.
5. A successful QoS 1 `publish()` means the library received PUBACK. The ESP32
   then sends `ACK,TELE,<seq>,SUCCESS` to the STM32.
6. The STM32 changes the CSV `sent` flag to `1`, deletes that queue record, and
   only then advances to the next oldest record.

The STM32-to-ESP32 snapshot is split into bounded UART frames named `BEGIN`,
`GWID`, `GWSTATUS`, `GWDC`, `GWAC1`, `GWAC2`, `GWENERGY`, `GWHEALTH`, `RELAY`,
and `END`. Every frame retains the same sequence number.

## Data quality and validation requirements

Before commissioning, compare firmware results with ModbusPoll for at least
the following operating states:

1. Daytime, normal on-grid generation.
2. Low-power sunrise/sunset operation.
3. Night/standby operation.
4. A disconnected or silent RS485 device.
5. Optional-register Modbus exception handling.

Validation must cover:

- raw request address and count;
- raw response words;
- U16/U32/S32 interpretation;
- high-word-first order;
- engineering scaling;
- MPPT power calculations;
- CSV column alignment;
- UART field alignment;
- MQTT key, type, and unit;
- handling of unsupported and stale values.

## Deferred work

Independent read/send web controls for Power Meter, GoodWe, and Sungrow are a
separate future change. This implementation retains the existing GoodWe web
control and does not add or remove PM/Sungrow controls.

## Source documents

- `GoodWe Grid-tied MTG2SMTSDTG2MSDNSXS Series Modbus Protocol-Costomer Version 20241022 (1).pdf`
- GoodWe SDT G2 Series User Manual for GW10KT-DT

