<p align="right">
  <a href="wifi-lab.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Wi-Fi Lab (Wi-Fi Lab / CTF)

`Wi-Fi Lab` is a tool-page sub-page that turns the device into a **raw 802.11 transmitter** for
**learning and authorized testing** of Wi-Fi stack behavior. It is the fourth radio role managed
by `main/net/app_net.c`, alongside the channel scanner, the provisioning path, and the BLE roles.

This module is a **faithful port of the user's own, publicly released GhostESP code** — specifically
the frames built in `main/managers/wifi_manager.c`. It does **not** invent any new attack logic,
does not improve range, stealth, target selection, or evasion. It reproduces the exact byte
templates, the exact randomization scheme, and the exact quirks (including a known off-by-two in
the beacon template) so the behavior matches GhostESP byte-for-byte. The point is to make those
well-known, publicly documented 802.11 patterns easy to reproduce **in a controlled, authorized
environment**, behind an explicit consent gate.

The module never decides "against whom" to transmit. The UI forces an explicit legal/authorization
acknowledgement before a single byte leaves the radio, and the consent flag lives only in page
memory and is cleared on exit.

## Authorized use only — read this first

Transmitting crafted 802.11 frames at networks or stations you do not own or are not authorized to
test can violate local radio-spectrum regulations (e.g. unlicensed-band transmission rules),
computer-misuse / anti-hacking laws, and platform terms of service, and can disrupt nearby users.
The module requires an explicit OK on a Chinese legal/authorization notice before any byte is
transmitted.

Use it only:

- on networks and devices you own, or
- on networks and devices you have written authorization to test, and
- in a lawful, authorized environment (e.g. a private lab with isolated equipment).

You are solely responsible for how you use the output. The authors and this port accept no
liability for misuse.

## Modes

The four top-level modes reproduce frames taken from GhostESP (see source notes below). The beacon
mode has three sub-modes.

| Mode | Frame | Payload size | Notes |
| --- | --- | ---: | --- |
| Deauth / Disassoc (`APP_WIFILAB_DEAUTH`) | Deauthentication (and Disassociation) | 26 B | AP→STA and STA→AP directions; reason code configurable |
| EAPOL Logoff (`APP_WIFILAB_EAPOL_LOGOFF`) | 802.1X EAPOL Logoff | 36 B | makes an associated station believe it was logged off |
| SAE Flood (`APP_WIFILAB_SAE_FLOOD`) | SAE Authentication (Commit) | 128 B | WPA3 handshake commit flood; scalar+element randomized |
| Beacon Spam (`APP_WIFILAB_BEACON_SPAM`) | Beacon | 38 + ssid_len + 12 + 3 + 13 | three sub-modes (see below) |

Beacon sub-modes (`app_wifilab_beacon_t`):

| Sub-mode | SSID source | Notes |
| --- | --- | --- |
| `APP_WIFILAB_BEACON_RANDOM` | random 8-char alphanumeric | random local MAC; broadcast on every channel 1..11 |
| `APP_WIFILAB_BEACON_RICKROLL` | Rickroll lyric (first 5 lines) | cycles through lyrics as SSID per cycle |
| `APP_WIFILAB_BEACON_AP_LIST` | SSIDs discovered by the channel scan | re-broadcasts nearby APs' SSIDs |

In `BEACON_SPAM` the transmitter walks channels 1..11 (matching GhostESP's
`wifi_manager_broadcast_ap` channel range). In `AP_LIST` it reuses the last channel-scan report
(`app_net_channel_report()`) instead of generating random SSIDs.

## Pure logic layer — `main/logic/app_wifilab.{c,h}`

This layer has **no ESP-IDF or LVGL includes** (only `<stdbool.h>`, `<stdint.h>`, `<stddef.h>`,
`<string.h>`). It is a pure "given a mode and a target, compute the exact bytes to put on the air"
library, so every frame can be verified on a host without disturbing any network. Actual
transmission is done by the `app_net` Wi-Fi role.

Exposed API:

- `app_wifilab_build_deauth(buf, cap, bssid, sta, reason, *out_len)` — Deauth (AP→STA).
- `app_wifilab_build_disassoc(buf, cap, bssid, sta, reason, *out_len)` — Disassoc (AP→STA).
- `app_wifilab_build_deauth_rev(buf, cap, bssid, sta, reason, *out_len)` — Deauth (STA→AP).
- `app_wifilab_build_disassoc_rev(buf, cap, bssid, sta, reason, *out_len)` — Disassoc (STA→AP).
- `app_wifilab_build_eapol_logoff(buf, cap, ap_bssid, sta_mac, *out_len)` — EAPOL Logoff.
- `app_wifilab_build_sae_commit(buf, cap, target_bssid, src_mac, frame_counter, *out_len)` — SAE Commit.
- `app_wifilab_build_beacon(buf, cap, sub, ssid, ssid_len, bssid, channel, *out_len)` — Beacon.
- `app_wifilab_rickroll_count(void)` / `app_wifilab_rickroll_line(index)` — Rickroll lyric set.

Every builder writes the real or needed length to `*out_len`. When `cap` is too small it returns
`APP_WIFILAB_ERR_BUF_TOO_SMALL` and writes the required length to `*out_len`. `NULL` buffer /
`out_len`, or a mandatory address of `NULL`, returns `APP_WIFILAB_ERR_INVALID_ARG`.

Deterministic PRNG (no `esp_random`):

- `app_wifilab_set_seed(uint32_t)` — set the module PRNG seed.
- `app_wifilab_xorshift32(uint32_t *state)` — standard xorshift32: `x ^= x<<13; x ^= x>>17; x ^= x<<5`.
  Same seed always yields the same sequence, which is what the host test relies on.
- `app_wifilab_random_mac(uint8_t out[6])` — random local unicast address; first byte is
  `& 0xFE | 0x02` (matching GhostESP's `generate_random_mac`).
- `app_wifilab_random_ssid(char *out, size_t len)` — random alphanumeric SSID using the same
  charset as GhostESP's `generate_random_ssid` (`ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789`,
  `RANDOM_SSID_LEN = 8`).

### Exact byte layouts (faithful to GhostESP)

- **Deauth / Disassoc (26 B):** `C0 00 3A 01` (Disassoc: `A0 00 3A 01`) + `dst[6]` + `src[6]` +
  `bssid[6]` + sequence control `[2]` (high 12 bits from PRNG) + `07 00` (reason 7, configurable).
  The `_rev` variants swap direction: dst = AP BSSID, src/BSSID = STA MAC.
- **EAPOL Logoff (36 B):** `08 01 00 00` + `dst = ap_bssid[6]` + `src = sta_mac[6]` +
  `bssid = ap_bssid[6]` + sequence `00 00` + LLC/SNAP `AA AA 03 00 00 00 88 8E` +
  EAPOL `01 02 00 00` (version 1, type Logoff, length 0).
- **SAE Commit (128 B):** `B0 00` + `dst = target_bssid[6]` + `src = src_mac[6]` +
  `bssid = target_bssid[6]` + sequence `[2]` = `(prng & 0xFFF0) | (frame_counter & 0x000F)` +
  auth body `03 00` (algorithm SAE) `01 00` (transaction = Commit) `00 00` (status 0)
  `13 00` (group 19) + scalar `[32]` + element `[64]` (both fully randomized from PRNG).
- **Beacon:** length `38 + ssid_len + 12 + 3 + 13`.
  - `80 00 00 00` (beacon) + `dst = ff:ff:ff:ff:ff:ff` + `src[6]` + `bssid[6]` +
    sequence `C0 6C` + timestamp `[8]` (zeroed) + beacon interval `64 00` + capability `11 04`.
  - SSID IE: `00` (tag) + `len` + `ssid[len]`.
  - Supported rates IE: `01 08 82 84 8B 96 24 30 48 6C` (GhostESP's source counts this block as
    12 bytes, which leaves **2 trailing `0x00` bytes** at the end of the frame — this is a real
    GhostESP quirk and is reproduced verbatim).
  - DS Parameter Set IE: `03 01 <channel>`.
  - HE IE: `FF 0D 50 6F 9A 00 08 00 00 40 00 00 01`.

Sources: GhostESP `main/managers/wifi_manager.c` — `wifi_manager_broadcast_deauth` (+ reverse
deauth/disassoc), `eapol_logoff_frame_template` + `eapol_logoff_task`,
`SAE_COMMIT_TEMPLATE` + `inject_sae_commit_frame`, and `wifi_manager_broadcast_ap` (beacon).

## Wi-Fi role — `Wi-Fi Lab` in `main/net/app_net.c`

The role shares the **single 2.4 GHz radio** with the channel scanner, the provisioning path, and
the BLE roles, and is wired into the **same arbitration**:

- `app_net_wifilab_start()` refuses when `app_ble_active()` (any BLE role up), when
  `app_net_channel_scan_running()` (Wi-Fi channel scan active), or when `app_net_prov_active()`
  (provisioning running). Reverse checks were added too: the channel scan request, provisioning
  start, and all three BLE roles (`app_ble_finder_start`, `app_ble_remote_start`,
  `app_ble_adv_start`) now refuse when `app_net_wifilab_running()` returns true. Only one of these
  radio users may be active at a time.
- Start/stop go through the **same async request/worker pattern** as BLE: the UI calls
  `app_net_wifilab_request_start(...)` / `app_net_wifilab_request_stop()`, which post
  `WIFILAB_REQ_START` / `WIFILAB_REQ_STOP` to a request queue; a resident `wifilab_ctrl_task`
  performs the (blocking) Wi-Fi bring-up / teardown serially so the LVGL thread never blocks.
- The same `last_error()` / `error_text()` pattern reports a one-line user reason
  (`app_net_wifilab_last_error`, `app_net_wifilab_error_text`).

Lifecycle in `wifilab_start_locked()`:

1. Run the three exclusion checks above; bail with `ESP_ERR_INVALID_STATE` + a human-readable
   reason if any radio user is busy.
2. `wifi_ensure_started()` then `esp_wifi_set_mode()` — `WIFI_MODE_AP` for every mode except
   `APP_WIFILAB_SAE_FLOOD`, which uses `WIFI_MODE_STA` (SAE Commit is sent from a STA interface).
3. Spawn `wifilab_attack_task`, which loops: `esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE)`
   then `esp_wifi_80211_tx(ifx, frame, len, false)`. Interface selection mirrors GhostESP:
   deauth/eapol/beacon use `WIFI_IF_AP`, SAE uses `WIFI_IF_STA`.
4. For `BEACON_SPAM`, the loop walks channels 1..11; `BEACON_AP_LIST` reuses the last scan report
   (`app_net_channel_report()`) for SSID/BSSID/channel instead of random values.
5. `wifilab_stop_locked()` stops the task, clears the active flag, and — only if the role had
   started Wi-Fi itself (`s_wifilab_was_started`) — releases the radio with the usual rollback.

Requested parameters are `app_net_wifilab_start(mode, beacon, target_bssid[6], target_channel,
target_ssid, target_ssid_len, seed)`. Every `esp_wifi_80211_tx()` return value is counted:
`app_net_wifilab_packets_sent()` counts only frames the driver accepted (`ESP_OK`),
`app_net_wifilab_packets_failed()` counts rejections, and `app_net_wifilab_tx_error_text()`
returns the first rejection's error name. The UI status line shows all three, so the frame count
can never be mistaken for proof that frames left the radio.

### Verified `esp_wifi` symbols (ESP-IDF 5.5.3)

`esp_wifi_80211_tx`, `esp_wifi_set_channel`, `esp_wifi_set_mode`, `esp_wifi_get_mac`,
`WIFI_IF_AP`, `WIFI_IF_STA`, `WIFI_MODE_AP`, `WIFI_MODE_STA`, `WIFI_SECOND_CHAN_NONE`. All were
confirmed present in `components/esp_wifi/include/`. Transmission uses `en_sys_seq = false` so the
sequence numbers we compute (and the tests assert) are the ones actually sent.

## UI — `main/ui/ui_wifilab.c`

Follows `ui_blelab.c`'s structure. Three views:

1. **Consent view (`WL_CONSENT`)** — shows the Chinese authorization/legal notice and requires an
   explicit OK before anything is transmitted. Consent is a page-memory flag cleared on exit.
2. **Lab view (`WL_LAB`)** — mode list (↑↓ to choose), beacon sub-mode / target line for the
   relevant modes, OK to start/stop, and a status line showing driver-accepted frames, rejected
   frames, and the first rejection's error name (`app_net_wifilab_packets_sent()` /
   `_packets_failed()` / `_tx_error_text()`). All Wi-Fi work is async
   (`app_net_wifilab_request_start/stop`); no `esp_wifi` calls happen under the LVGL lock.
3. **Target view (`WL_TARGET`)** — for target-taking modes, reuses `app_net_channel_scan_request()`
   to scan and present the discovered AP list (capped at `WL_MAX_APS = 7`) so the user can pick a
   BSSID/channel/SSID to act on. Row count is clamped so key handling never indexes past the list.

Registered in `main/ui/ui_tools.c` as `TOOL_WIFILAB` (name `WiFi Lab`, hint
`CTF / lab wireless testing`), added to all five `subpage_*` switches, and declared in
`main/ui/ui_pages.h`.

## Host tests

`tests/test_app_wifilab.c` asserts the exact bytes/lengths of every mode (deauth, disassoc, reverse
deauth, EAPOL logoff, SAE commit with deterministic random region, beacon with random SSID/MAC on
channel 6 plus the trailing 2 `0x00` bytes, and a Rickroll-lyric beacon), the "buffer too small
returns needed length" behavior, invalid-arg handling, xorshift32 determinism (same seed → same
sequence, different seed differs), and the Rickroll lyric set (5 lines, modulo wrap). It depends
only on the standard library and `logic/app_wifilab.h` — no ESP-IDF/LVGL — and is compiled and run
by `tools/validate.sh --static` (added to the `wifilab` logic test entry).

`tests/test_net_contract.py` additionally pins the transmit-counting contract: every
`esp_wifi_80211_tx()` call site must route its result through `wifilab_count_tx()`, and both
counters may only be incremented there — so a later edit cannot silently turn the sent-frame count
back into a loop count.

## Build caveats — `esp_wifi` / `sdkconfig`

- **802.11 raw TX** uses `esp_wifi_80211_tx`, which ESP-IDF 5.5.3 provides unconditionally on
  ESP32-C3: the IDF 4.x options `CONFIG_ESP_WIFI_80211_TX_ENABLED` /
  `CONFIG_ESP_WIFI_ENABLE_WIFI_TX` no longer exist anywhere under `components/` (only the
  unrelated `ESP_WIFI_ENABLE_WIFI_TX_STATS` remains), so a constraining Kconfig gate is not a
  possible cause here. Whether a given frame is accepted is a runtime property of the driver and
  chip; the module records the `esp_wifi_80211_tx()` return value instead of assuming success.
- **Wi-Fi mode support**: `WIFI_MODE_AP` and `WIFI_MODE_STA` must both be available. Targets that
  are STA-only (some low-end ESP32 variants / certain power configurations) cannot run the
  `WIFI_IF_AP` beacon/deauth paths. GhostESP restricts SAE flood to C5/C6-class chips; this port
  uses a more generic platform abstraction and does **not** do per-SoC gating, so on a
  non-AP-capable target the AP-mode modes will fail at `esp_wifi_set_mode`. Confirm the target
  supports AP + STA concurrent/independent modes.
- **Region / TX power**: transmitting crafted frames continuously may exceed local duty-cycle or
  power limits; this is a deployment/compliance concern, not a build one.
- The module does not call `esp_random`, so no extra entropy source configuration is needed for the
  logic layer; the host test exercises the deterministic PRNG only.
