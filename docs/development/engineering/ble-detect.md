<p align="right">
  <a href="ble-detect.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# BLE Detect (passive BLE detection)

`BLE Detect` is a tool-page sub-page that **listens only** to nearby BLE advertising and classifies
what it hears. It is a defensive/passive port of the ideas in GhostESP's
`detect_ble_spam_callback` and `airtag_scanner_callback`: it never transmits, never spoofs, never
disconnects, and never runs an active radio role. It reuses the **finder (observer scan)** already
managed by `main/net/app_ble.c` — there is no second NimBLE role and no second stack.

## Passive / defensive only — read this first

This module is a detector, not an attacker. It:

- only reads **public** advertising data that devices broadcast anyway,
- performs a heuristic classification and prints a human-readable verdict,
- transmits **nothing**.

It makes no identity claim about any device: BLE MAC addresses rotate, most devices do not advertise
a name, and the manufacturer/service patterns are conservative "looks like" signals. The verdicts
are advisory ("this looks like a flood", "there is an AirTag-like device near you"), never proof.

Use it only to understand your own surroundings. Do not use it to harass, track, or interfere with
people you have no business observing.

## What it detects

Three verdicts, computed from the stream of observed advertisements:

1. **Apple Continuity spam** — many different advertisers, all Apple (`company_id 0x004C`) whose
   manufacturer-data prefix matches a known Continuity payload:
   - `4C 00 07 19 07` (air payload `1e ff 4c 00 07 19 07`), and
   - `4C 00 04 04 2a 00` (air payload `16 ff 4c 00 04 04 2a 00`).

   Reported as "suspected BLE spam flood" when the count of distinct such sources seen inside the sliding
   window reaches `APP_BLEDETECT_SPAM_THRESHOLD` (8).
2. **AirTag / Find My presence** — an Apple advertiser whose manufacturer data matches the Find My
   service data `12 19`, or the `07 19` pairing pattern, or which broadcast the Find My network
   service UUID `0xFD5A`. Reported as "AirTag / Find My device found".
3. **Other / unknown advertisers** — a count of sources that are neither a spam candidate nor an
   AirTag (includes non-Apple devices and Apple devices without a known pattern).

Verdict priority: spam beats AirTag beats normal.

## Where the manufacturer / service data comes from

The pure-logic detector needs the raw manufacturer-data bytes to match the Continuity prefixes, but
`app_finder_t` (the snapshot the finder exposes) originally carried **only the heuristic category**,
not the underlying identifiers. So the finder snapshot was extended (in `main/logic/app_finder.h` /
`.c`) to carry, per device:

- `company_id` (16-bit), `mfg_type` (first byte after the company id) — written by `app_finder_feed`
  from its existing parameters (no signature change),
- `mfg_data[APP_FINDER_MFG_BYTES]` + `mfg_data_len` — the **leading** bytes of the manufacturer
  specific data field (NimBLE `fields.mfg_data`, with the 2-byte little-endian company id at the
  front). Full data can be up to 31 bytes; 8 leading bytes are enough to match every prefix this
  detector needs, so we store only that. Written by the new helper `app_finder_attach_mfg()`,
- `svc16[APP_FINDER_SVC_MAX]` + `svc16_count` — the 16-bit service UUID list, copied from the
  `uuid16` parameter already passed to `app_finder_feed`.

The scan callback in `main/net/app_ble.c` (`finder_gap_event`) now calls `app_finder_attach_mfg()`
right after `app_finder_feed()`, under the same lock, so the snapshot the UI polls already contains
everything the detector needs. No new BLE role, no duplicated stack.

### On "rate"

The UI polls a finder snapshot (de-duplicated by address, ~800 ms cadence) and feeds each device to
the detector every tick. The detector therefore counts **distinct Apple Continuity sources observed
within the sliding window** rather than raw packet count. For randomized-MAC floods — where every
packet is a new address — this still saturates and trips the threshold; for a handful of legitimate
Apple devices it stays at a low, stable number. This is a deliberate adaptation of GhostESP's
per-packet rate counter to the snapshot-based UI, and is documented as heuristic.

## Pure logic layer — `main/logic/app_bledetect.{c,h}`

No ESP-IDF or LVGL includes (only `<stdbool.h>`, `<stdint.h>`, `<stddef.h>`, `<string.h>`). It
exposes:

- `app_bledetect_reset()` — clear all internal state (call on page enter).
- `app_bledetect_feed(const app_bledetect_ad_t *)` — feed one observed advertisement. `now_ms` must
  be monotonic; repeated addresses refresh their window timestamp and merge category flags.
- `app_bledetect_report(app_bledetect_report_t *)` — compute the verdict + counts + a few example
  addresses from the current sliding window.
- `app_bledetect_verdict_text()` / `app_bledetect_kind_text()` — Chinese labels for the UI.

Everything is driven by the fed `(address, timestamp)` pairs; there is **no `esp_random`**, so the
host test is fully deterministic.

## UI — `main/ui/ui_bledetect.c`

Mirrors `ui_finder.c` / `ui_blelab.c`:

- `enter` — `app_bledetect_reset()`, then `app_ble_finder_request_start()` (async; never brings up
  the stack under the LVGL lock).
- `exit` — `app_ble_finder_request_stop()` then delete the screen.
- `tick` (every `BD_TICK_MS = 800`) — poll `app_ble_finder_snapshot()`, feed each device into the
  detector, render the report.
- `key` — long OK returns to the tools page; short OK pauses/resumes listening (receive only).

The screen shows: a passive-only banner ("passive listening only, no transmission"), a verdict line (normal /
spam / AirTag, color-coded), a counts line, and a small list of up to `APP_BLEDETECT_EG_MAX`
example addresses. It reuses `ui_list_create` / `ui_row_create` / `ui_page_set_hint`.

Registered in `main/ui/ui_tools.c` as `TOOL_BLEDETECT` (name `BLE Detect`, hint `is Bluetooth spam flooding nearby?`),
added to all five `subpage_*` switches, and declared in `main/ui/ui_pages.h`.

## Build / test wiring

- `main/CMakeLists.txt` lists `logic/app_bledetect.c` and `ui/ui_bledetect.c`.
- `tools/validate.sh` includes `bledetect` in the `--static` logic-test loop, compiling and running
  `tests/test_app_bledetect.c` (with `main/logic/app_bledetect.c` + `main/logic/app_text.c`).

## Host tests

`tests/test_app_bledetect.c` asserts, with deterministic feeds, each verdict: a normal environment
(non-Apple sources), AirTag / Find My (both the `12 19`/`07 19` manufacturer patterns and the
`0xFD5A` service UUID), Apple Continuity spam (both known prefixes, above threshold), the "not enough
sources" non-false-positive case, sliding-window decay, and verdict-text strings. It is compiled and
run by `tools/validate.sh --static`.
