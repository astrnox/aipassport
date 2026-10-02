<p align="right">
  <a href="ble-lab.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# BLE Lab (BLE Advertising Lab)

`BLE Lab` is a tool-page sub-page that turns the device into a **broadcaster** — it emits
raw BLE advertising payloads and never connects to anything. It is the third BLE role
managed by `main/net/app_ble.c`, alongside the finder (observer scan) and the remote
(HID peripheral).

The point of the module is to make well-known, publicly documented BLE advertising
formats easy to reproduce for **learning and authorized testing**, behind an explicit
consent gate. It is deliberately inert until the user confirms they understand the legal
boundary.

## Authorized use only — read this first

Broadcasting crafted packets at devices you do not own or are not authorized to test can
violate local radio-spectrum regulations and platform terms of service, and can annoy or
disrupt nearby users. The module requires an explicit OK on a Chinese legal/authorization
notice before any byte is transmitted; the consent flag lives only in page memory and is
cleared on exit.

Use it only:

- on devices you own, or
- on devices you have written authorization to test, and
- in a lawful, authorized environment.

You are solely responsible for how you use the output.

## Modes

All four modes reproduce payloads taken from public repositories (see source notes below):

| Mode | Payload size | Notes |
| --- | ---: | --- |
| Apple audio (`APPLE_AUDIO`) | 31 B | AirPods-style continuous popup; `modelId` at index 7 |
| Apple setup (`APPLE_SETUP`) | 23 B | AppleTV/HomePod-style setup popup; `modelId` at index 13 |
| Swift Pair (`SWIFT_PAIR`) | 10 B (+ optional name) | Windows Swift Pair; configurable Class of Device + name |
| iBeacon (`IBEACON`) | 27 B | Apple iBeacon service data; configurable UUID/major/minor/tx |

For the Apple modes the advertiser rotates through the device table one entry per cycle,
so the popup targets keep changing. Swift Pair and iBeacon keep a fixed identity (set via
their setters) and simply re-broadcast it.

## Pure logic layer — `main/logic/app_blelab.{c,h}`

This layer has **no ESP-IDF or LVGL includes** (only `<stdbool.h>`, `<stdint.h>`,
`<stddef.h>`, `<string.h>`). It exposes:

- `app_blelab_build_packet(mode, index, buf, cap, *out_len)` — fills `buf` and reports the
  real/needed length. When `cap` is too small it returns `APP_BLELAB_ERR_BUF_TOO_SMALL`
  and writes the required length to `*out_len`.
- `app_blelab_packet_size(mode)` — the fixed size for a mode.
- Apple device table accessors (`app_blelab_apple_count`, `app_blelab_apple_dev`).
- Setters: `app_blelab_set_ibeacon(...)`, `app_blelab_set_swift_cod(...)`,
  `app_blelab_set_swift_name(...)`.
- Deterministic PRNG helpers: `app_blelab_xorshift32(state)` and
  `app_blelab_random_addr(state, out[6])` (first byte OR `0xF0`, i.e. a random static
  address). No `esp_random` — the same seed always yields the same sequence, which is what
  the host test relies on.

### Exact byte layouts

- **Apple audio (31 B):** `1e ff 4c 00 07 19 07` + `modelId`@7 +
  `20 75 aa 30 01 00 00 45 12 12 12` (bytes 8–18), remaining bytes zero-padded to 31.
- **Apple setup (23 B):** `16 ff 4c 00 04 04 2a 00 00 00 0f 05 c1` (13 B) + `modelId`@13 +
  `60 4c 95 00 00 10 00 00 00` (9 B).
- **Swift Pair (10 B):** `09 ff 06 00 03 02 80` + 3 CoD bytes (little-endian). Optional
  `Complete Local Name` appended as `len 09 <name...>`.
- **iBeacon (27 B):** `1a ff 4c 00 02 15` + 16-byte UUID + major (BE) + minor (BE) + tx
  power (signed).

Sources: `EvilAppleJuice-ESP32/src/devices.cpp` & `devices.hpp` (Apple),
`ESP32-SwiftSpam/ESP32-SwiftSpam.ino` (Swift Pair), `ESP32-BLEBeaconSpam/*.ino` (iBeacon).

## BLE role — advertiser in `main/net/app_ble.c`

The advertiser shares the single BLE stack with finder and remote and is wired into the
**same arbitration**:

- A single `ble_mode_t` (`BLE_MODE_IDLE` / `BLE_MODE_FINDER` / `BLE_MODE_REMOTE` /
  `BLE_MODE_ADV`) allows only one role at a time.
- `app_ble_adv_start()` refuses when `s_mode != BLE_MODE_IDLE` (finder/remote busy),
  when `app_ble_prov_active()` (provisioning running), or when
  `app_net_channel_scan_running()` (Wi-Fi channel scan). And vice-versa: finder/remote
  refuse while the advertiser is up, and provisioning / channel scan refuse because
  `app_ble_active()` returns true whenever any role — including the advertiser — is active.
- Start/stop go through the **same async request/worker pattern** (`BLE_REQ_ADV_START` /
  `BLE_REQ_ADV_STOP` posted to `s_req_q`), so the UI never blocks on stack bring-up.
- The same `last_error()` / `error_text()` pattern reports a one-line user reason.

Lifecycle mirrors the rest of the file: `app_ble_adv_start()` sets `s_mode = BLE_MODE_ADV`
and calls `stack_up()`; the actual advertising happens in the `on_sync` callback
(`adv_start_late`), which sets a **random static address** via `ble_hs_id_set_rnd()`
(seeded by `app_blelab_random_addr`), pushes the first raw packet with
`ble_gap_adv_set_data()`, and starts **non-connectable, general-discoverable** advertising
(`BLE_GAP_CONN_MODE_NON`, `BLE_GAP_DISC_MODE_GEN`). A periodic `esp_timer` cycles the
payload (Apple modes rotate the device) by calling `ble_gap_adv_set_data()` again.
`app_ble_adv_stop()` stops the timer, stops advertising, and calls `stack_down()` with the
usual rollback.

### Verified NimBLE symbols (ESP-IDF 5.5.3)

`ble_gap_adv_set_data`, `ble_gap_adv_start`/`ble_gap_adv_stop`,
`BLE_GAP_CONN_MODE_NON`, `BLE_GAP_DISC_MODE_GEN`, `BLE_GAP_EVENT_ADV_COMPLETE`,
`ble_hs_id_set_rnd`, `BLE_OWN_ADDR_RANDOM`, `ble_hs_util_ensure_addr`. The broadcaster
role is enabled in `sdkconfig.defaults` (`CONFIG_BT_NIMBLE_ROLE_BROADCASTER=y`), so no
sdkconfig change is required.

## UI — `main/ui/ui_blelab.c`

Follows `ui_finder.c`'s structure. Two views:

1. **Consent view** — shows the Chinese authorization/legal notice and requires an explicit
   OK before anything is transmitted. Consent is a page-memory flag cleared on exit.
2. **Lab view** — mode list (↑↓ to choose), OK to start/stop, status line, and hints via
   `ui_page_set_hint`. All BLE work is async (`app_ble_adv_request_start/stop`); no NimBLE
   calls happen under the LVGL lock.

Registered in `main/ui/ui_tools.c` as `TOOL_BLELAB` (name `BLE Lab`), added to all five
`subpage_*` switches, and declared in `main/ui/ui_pages.h`.

## Host tests

`tests/test_app_blelab.c` asserts the exact bytes/lengths of every mode, the "buffer too
small returns needed length" behavior, invalid-arg handling, the Apple device table, the
iBeacon/Swift Pair setters, and PRNG determinism. It is compiled and run by
`tools/validate.sh --static`.
