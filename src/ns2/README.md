# Switch 2 protocol core provenance

The protocol core in this directory is based on PicoAmiibo-RP2040 by LockingFR,
from commit `973d52830a084b8ee411d6c55429dd7023a33f7e`:

https://github.com/LockingFR/PicoAmiibo-RP2040

The upstream project is licensed under Apache-2.0. Its license text is retained
in `LICENSE`. `identity.c` also preserves its source attribution to PicoSwitch2
v2.5.0. PicoAmiibo records that source commit as
`cdb5bf6f85cef9998c11da280073a0c7642b712c` and identifies Apache-2.0 licensing.

This port moves the pure C protocol modules into `src/ns2/` and their public
headers into `include/ns2/`. Local changes add direct RAM loading of raw 540-byte
and 572-byte Amiibo dumps, stick-axis packing, a successful NFC data-read
counter, C++ linkage guards, and this provenance note. Reads remain supported;
tag writes are refused by the device adapter. Pico flash slots, persistent
storage, GPIO/BOOTSEL handling, desktop-manager code, and game data are excluded.
