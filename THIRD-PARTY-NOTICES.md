# Third-party notices

## PawnIO.Modules: `LpcACPIEC.bin`

- Project: https://github.com/namazso/PawnIO.Modules
- Version: 0.2.11 (official signed build, unmodified)
- Release archive: https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip
  (SHA-256 `43608CB89BC84247FEF1368A139013F7D043E17DB6D6C8DFC9B46BF0905A81F4`)
- `LpcACPIEC.bin` SHA-256: `C38FD116E7AFF4D1FDB0A494E296BE0A6708E5A22FC72F14587442FB7F8F7906`
- Source: https://github.com/namazso/PawnIO.Modules/tree/0.2.11 (`LpcACPIEC.p`)
- License: GNU Lesser General Public License v2.1. The full text is in
  [third_party/PawnIO.Modules/COPYING](third_party/PawnIO.Modules/COPYING) and in the release zip.

Laptop Fan Curve loads this module at run time through the PawnIO driver's public interface. The module is
distributed unmodified. Its source is available at the link above.

## PawnIO driver

PawnIO (https://pawnio.eu, https://github.com/namazso/PawnIO, GPL-2.0) is **not** included. Users install it
separately.

## NoteBook FanControl configs

No NBFC configs are included. Users import configs from https://github.com/hirschmann/nbfc or
https://github.com/nbfc-linux/nbfc-linux under those projects' licenses. Laptop Fan Curve implements a
reader for the published config format.

## Factory fan tables

`src/FactoryTables.h` contains 480 bytes of numeric fan-table values (temperature thresholds and fan speeds)
for the Acer Swift X 16 SFX16-61G, read from that laptop's firmware for interoperability. No firmware code is
included.
