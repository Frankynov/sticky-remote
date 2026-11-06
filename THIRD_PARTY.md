# Third-party components

The MIT license in `LICENSE` covers this project's own code. These parts keep
their own licenses.

| Component | Where | License |
|---|---|---|
| [Inter](https://rsms.me/inter/) Bold, by Rasmus Andersson | `firmware/fonts/Inter-Bold.ttf` | SIL OFL 1.1 (`firmware/fonts/OFL-1.1.txt`) |
| TRMNL12, TRMNL16, TRMNL21, by Heavyweight Digital Type Foundry for [TRMNL](https://trmnl.com/font-family) | `firmware/fonts/trmnl/` | SIL OFL 1.1 (`firmware/fonts/trmnl/OFL.txt`, `NOTICE.md`) |
| [Material Design Icons](https://pictogrammers.com/library/mdi/), by Pictogrammers | `firmware/fonts/materialdesignicons-webfont.ttf` | Apache 2.0 (`firmware/fonts/Apache-2.0.txt`) |
| [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library), by Project Nayuki | `engine/core/qrcodegen/` | MIT (`engine/core/qrcodegen/LICENSE.md`) |
| [esptool-js](https://github.com/espressif/esptool-js) 0.7.1, by Espressif | `web/vendor/` | Apache 2.0 (`web/vendor/esptool-js-LICENSE.txt`) |

The glyph files in `web/fonts/` and the images in `web/previews/` are generated
from the fonts above and keep their licenses.

**Firmware binaries** built from this repository include
[ESPHome](https://esphome.io)'s runtime and Espressif's ESP-IDF. ESPHome's C++
runtime is licensed under GPLv3, so a distributed `.bin` is subject to its
terms; the source to rebuild it is this repository plus ESPHome.
