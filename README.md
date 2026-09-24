<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Passport Toolbox

An offline-first everyday toolbox for the **FoloToy AI Passport** wearable
(ESP32-C3, 240 × 320 portrait LCD, three physical buttons). It is a derivative
application built on the repository's hardware-test baseline: the reusable board
logic in `components/bsp` is kept, while the application screens, navigation,
visual design, and button behaviour are designed from scratch for this product.

The product goal is simple: the device must stay useful when there is no network.
Calendar, routine countdowns, Pomodoro focus, badge, and the offline TOTP
passcode all work with no connection at all. The only online module is the
League of Legends esports centre, and it degrades to a clearly labelled local
cache when the network is unavailable.

The full requirements, interaction rules, and acceptance criteria are in the
[product requirements document](docs/product/passport-toolbox-prd.md); a
scrollable, interactive screen prototype ships beside it as
[`passport-toolbox-ui-prototype.html`](docs/product/passport-toolbox-ui-prototype.html).

## What the application does

| Area | Highlights |
| --- | --- |
| Home | Three always-visible information cards (clock, next routine node countdown, live or next followed match) above the six-module list, with a status bar and a per-page button hint bar. |
| Time and calendar | Gregorian calendar with Chinese lunar date, solar term and daily advice; a five-scale time-progress view (day / week / month / year / life); stopwatch and countdown timer. |
| Focus and efficiency | Pomodoro timer with configurable focus and break lengths and power-loss-safe records; up to 16 local reminders with weekday repeat. |
| Routine and countdown | Per-weekday schedule nodes with odd/even week support, current-node highlighting, and a large countdown to the next node. |
| Identity and tools | Up to five badge cards with an offline QR code; offline RFC 6238 TOTP showing the current code, its remaining validity, and the next code; hardware self-test. |
| LoL esports centre | Today's and this week's schedule, live scores, standings, teams, and per-game detail, ordered by a fixed priority and served from cache first. |
| System | Wi-Fi provisioning (SoftAP web page and BLE), time sync, screen-off and power saving, theme, sound, followed teams, and data backup/erase. |

## Offline-first design

- Every screen except the esports centre and time sync is reachable and usable
  with no network. There is no "connect to continue" blocking page.
- All user data (badges, routine, reminders, Pomodoro state, followed teams,
  TOTP secrets, esports cache) is stored locally in NVS.
- The esports centre renders the cached result first, then refreshes in the
  background, and labels the data freshness when offline.
- The interface is Chinese; the application ships a generated CJK bitmap font
  and a coverage check so no label or user-supplied name renders as a missing
  glyph.

## Hardware capability contract

| Capability | Confirmed implementation |
| --- | --- |
| Display | ST7789P3, 240 × 320 portrait RGB565, SPI, LEDC backlight |
| Input | `UP`, `DOWN`, `OK` on one ADC resistor ladder (GPIO0); no combination keys |
| Audio | ES8311 codec over I2S, playback and capture |
| Battery | CW2017 fuel gauge (state of charge and voltage) |
| Wi-Fi | 2.4 GHz STA, used only by the esports centre, provisioning, and time sync |
| Bluetooth LE | NimBLE peripheral, used only for provisioning |
| Storage | NVS on 8 MB Flash, no PSRAM |

All pins, addresses, and panel parameters are read from
[`components/bsp/include/bsp_pins.h`](components/bsp/include/bsp_pins.h);
application code does not duplicate hardware constants.

## Build and test

ESP-IDF **5.5.3** for ESP32-C3 is required. See the
[environment bootstrap](docs/development/engineering/environment-setup.md) and
[build and test](docs/development/engineering/build-and-test.md) guides.

```bash
./tools/validate.sh --static     # repository checks + host-side logic tests
./tools/validate.sh --firmware   # ESP-IDF build + merged-image verification
./tools/validate.sh              # complete gate (requires activated ESP-IDF 5.5.3)
```

The firmware gate produces the verified merged image at
`build/FoloToy-AI-Passport-full.bin`, which is flashed at offset `0x0`.

A successful build is not hardware validation. Report build results, host-test
results, device-test results, and unverified checks separately.

## Project structure

```text
components/bsp/   Reusable board support (display, buttons, audio, battery, shared I2C)
main/             Application: pure logic, screens, navigation, tasks, assets
assets/fonts/     Generated CJK bitmap fonts and their reproducible sources
tests/            Host-side logic tests that run without hardware
tools/            Shared local/CI validation scripts
docs/product/     Product requirements document and UI prototype
```

## Documentation

| Resource | Contents |
| --- | --- |
| [Product requirements](docs/product/passport-toolbox-prd.md) | Full feature, interaction, and acceptance specification |
| [AI agent guide](docs/development/ai-guide.md) | Development workflow and runtime invariants |
| [Chinese fonts](docs/development/engineering/lvgl-chinese-fonts.md) | Font generation, integration, and glyph-coverage checks |
| [Hardware guide](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) | Pin map, buses, and acceptance checklists |
| [Documentation index](docs/README.md) | Full documentation map |

## License

MIT, following the upstream repository. See [LICENSE](LICENSE).
