# How Laptop Fan Curve works

This page is for contributors. It explains how the app reaches the fans, how Identify finds a laptop's
fan settings, and where to start when adding support for a laptop.

## Source layout

| File | Contents |
|---|---|
| `src/LaptopFanCurve.cpp` | Direct2D UI, the control loops, commands and the Identify screen |
| `src/EcLive.h` | Embedded-controller (EC) access through PawnIO; Acer WMI (profiles, telemetry); ACPI thermal zone |
| `src/ModelDef.h` | Model definitions: the built-in model and the `.ini` format |
| `src/FactoryTables.h` | Factory fan tables for the built-in SFX16-61G model |
| `src/Identify.h` | Read-only discovery: SMBIOS, DSDT parsing, WMI classes, BIOS table scan, reports |
| `src/Nbfc.h` | NoteBook FanControl config reader (XML and JSON) and the control engine |

## EC access

All hardware access goes through the signed PawnIO driver and its `LpcACPIEC` module. Port access uses the
standard ACPI EC ports (`0x62` data, `0x66` command/status). Before each transaction the app waits for IBF,
OBF and SCI_EVT to be clear. It holds the `Global\Access_EC` mutex, the convention other EC tools share, and
retries timeouts up to three times.

- Standard EC space: `0x80` read and `0x81` write, used by NBFC configs.
- Acer extended RAM: `0x92` read and `0x93` write, each with a 16-bit address. The commands come from the model definition.

## Backend 1: Acer EC fan tables

The Acer EC runs its fan loop from a 120-byte table in extended RAM: 12 rows of 10 bytes, at `0x0800` on
the SFX16-61G.

```
row: [index, flags, cpuUp, cpuDown, gpuUp, gpuDown, cpuRpm (BE16), gpuRpm (BE16)]
```

- Row 0 holds the fans-off stage. Its `up` value is the temperature at which the fans start.
- In row *n*, `up` is the temperature for stepping up to row *n+1*, and `down` is the temperature for dropping back.
- The last active stage has `up = 0x7F`. The rows after it are sentinels (`0x7F`, 8000 rpm).
- CPU and GPU thresholds are separate columns, and each fan follows its own column.

Switching modes in AcerSense (WMI `APGeAction.SetFunction` with function 7) or sleeping makes the EC reload
its built-in table for the current mode. The app therefore keeps a canary byte at `0x0877`, which differs
from factory by one bit in an unreachable or +1 rpm field. It polls that byte every 2 s and writes the custom
table again when it changes back.

**Writes** change only the bytes that differ, and every byte is verified by reading it back. Each big-endian
RPM pair is written in the order that never briefly lowers cooling. **Undo** is Acer's own WMI command for
the current mode, which reloads the factory table.

Before writing a mode for the first time, the app compares the live table with that mode's factory table. A
mismatch disables custom curves for that mode.

## Backend 2: NBFC configs

`Nbfc.h` implements the documented NBFC semantics:

- Thresholds are sorted by `UpThreshold`. The fan steps up when `temp >= next.Up` and steps down when
  `temp < current.Down`.
- Speed is linear between `MinSpeedValue` and `MaxSpeedValue`, rounded. A `FanSpeedPercentageOverride`
  takes precedence if it is within 0.06%.
- Critical mode starts when `temp > CriticalTemperature` and ends when
  `temp < CriticalTemperature - CriticalTemperatureOffset` (default 15). It forces 100%.
- With `ReadWriteWords`, values are 16-bit little-endian at `reg` and `reg+1`.
- `RegisterWriteConfigurations` run `OnInitialization` or `OnWriteFanSpeed`, with `Set`, `And` or `Or`.
- On stop, the fans' `FanSpeedResetValue` and the registers' `ResetValue` are written, which returns control to the firmware.
- Configs that use `AcpiMethod` or `LuaCode` are rejected.

The temperature comes from Acer WMI when present. Otherwise it is the highest ACPI thermal zone
(`MSAcpi_ThermalZoneTemperature`). The engine stops if the temperature is unavailable for 5 consecutive
polls, after 5 EC errors, and when the app closes.

## Identify

1. **Fingerprint (no EC access).** SMBIOS strings; the DSDT read via `GetSystemFirmwareTable('ACPI','DSDT')`
   and parsed for `OperationRegion` and `Field` declarations (the EC device, memory windows, and fan or
   temperature field names); and the vendor WMI classes in `ROOT\WMI`.
2. **BIOS scan.** Looks for 12×10-byte tables that match the Acer layout rules above. `.exe`, `.zip` and
   `.7z` files are unpacked with 7-Zip, two levels deep. Consecutive tables at a 120-byte stride form a
   group. The first four are mapped to AcerSense codes Silent 2, Normal 0, Performance 3 and Turbo 4.
   A draft is only made when the DSDT shows the verified Acer EC layout: an EC device whose shared-memory
   window (a SystemMemory region at `0xFE70xxxx`) contains the `FNLV` fan-level field (`AcerEcFamily`).
   Other laptops never receive Acer's private EC commands; they get a report.
3. **Confirm.** One consented read of the live table, matched against the BIOS tables. If the live table
   belongs to a different slot, the mode mapping is corrected. For NBFC, Confirm reads the fan registers once.
   The result is stored in local settings as `confirmed_<fingerprint>`. `ModelFingerprint` hashes the identity,
   EC commands, table address, mode mapping and tables (or the NBFC config bytes). A `confirmed` value inside an
   `.ini` file is ignored. NBFC consent is stored per fingerprint as well.

Definitions are saved as `%LOCALAPPDATA%\LaptopFanCurve\models\<product>.ini`. Files in a `models` folder
next to the exe are loaded too, so tested definitions can be shipped.

## Adding a laptop

- **An Acer laptop that Identify handles:** attach the generated `.ini` and the report to an issue. Say
  whether Confirm and Apply worked, and whether the fans responded.
- **A different Acer EC layout, or another vendor:** start from the report. Its ACPI fields often name the
  fan and temperature registers. New backends should follow the same rules: verify before the first write,
  keep a factory restore, and fail safe to firmware control.

## Data locations

`%LOCALAPPDATA%\LaptopFanCurve\`: `custom_<mode>.bin` (custom Acer tables), `settings.ini` (switches, NBFC
curves, verification state), `models\` (definitions created by Identify), `log.txt`.
