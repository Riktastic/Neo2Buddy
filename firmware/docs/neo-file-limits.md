# AlphaSmart NEO File Limits

The NEO protocol exposes the size allocated to each document through file
attributes (`alloc_size`) and exposes free device memory through
`REQUEST_GET_AVAIL_SPACE` (`0x1a`). A complete restore or upload endpoint must
use those values immediately before writing, because the limits can vary with
the installed SmartApplet and the NEO's available RAM.

The upstream `neotools` implementation rejects a new file when its encoded NEO
content plus a 1 KiB transfer margin exceeds `free_ram`. Text is encoded and
padded before the comparison, so UTF-8 input length alone is not sufficient.

The current portal uses a 64 KiB local-backup warning only as an early visual
signal. It is not a device limit. Replace it with per-file `alloc_size`,
`min_size`, and current `free_ram` values when `neo_file` and the corresponding
authenticated HTTP endpoints are implemented.

## Battery Telemetry

The AlphaSmart NEO2 communications protocol used by `neotools` contains no
battery-level or voltage request. A NEO2 battery reading cannot be obtained
through its USB COM protocol. The ESP32 can measure its own supply only if the
DevKit is wired to a battery divider and ADC input; that is unrelated to the
NEO2's AA battery state.