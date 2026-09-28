<p align="right">
  <a href="memory-and-power-optimization.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Memory and Power Optimization Plan

Status: **Memory Stage 1 and Power Stage 1 implemented; Power Stage 2 enabled for
dynamic frequency scaling only.** The low-risk memory reductions below ship, the
Stage 1 measurement harness is now wired into the application
([`app_metrics.c`](../../../main/app_metrics.c)), and the "stop redrawing while
asleep" change is in. Dynamic frequency scaling (DFS) is enabled; automatic light
sleep is deliberately **not** enabled and stays deferred until the on-device
checks under "Power optimization plan" pass. Every number still marked "estimate"
must be replaced by a measured value from a device run before it is treated as a
result.

## Implemented so far

These changes target the "memory full" prompts seen during esports fetch,
channel scan, and time sync / provisioning:

- LVGL pool `CONFIG_LV_MEM_SIZE_KILOBYTES` 48 → 40, returning 8 KB to the
  system heap.
- Routine page releases a tab's widgets (`lv_obj_clean`) when it is hidden and
  rebuilds on show, so four tabs no longer coexist.
- `ui_row_create()` draws selection as a left border on the row instead of a
  separate child object, and `ui_row_t` stores the title handle directly.
- HTTP JSON responses are read into a buffer sized from the response (or grown
  from 4 KB), instead of a fixed `NET_HTTP_MAX_BODY` (64 KB) allocation.
- The esports schedule list is paginated (10 rows per page); the teams tab shows
  a win-rate bar built from block glyphs, adding no objects.
- `app_input` task stack raised 4 → 8 KB, and `ui_theme.c` helpers NULL-check
  widget creation.

Measurement harness (Memory Stage 1) and Power Stage 1:

- [`app_metrics.c`](../../../main/app_metrics.c) logs one line per sample point:
  LVGL pool free / fragmentation / peak use, internal heap free / largest block,
  and the `app_input` and LVGL task stack high-water marks. It is called on
  module enter and exit, on every routine tab switch, every 60 s while awake, and
  each transient network worker logs its own stack watermark on exit.
- While the screen is asleep, the 1 s tick no longer runs the periodic battery /
  status-bar refresh; `wake_now()` refreshes once on wake so the bar is not stale.
  The one place that used to open an alert while asleep without waking first
  (`report_missed_reminders()`) now wakes the screen, keeping the invariant
  "nothing touches the hidden screen while asleep" true.
- Dynamic frequency scaling: `CONFIG_PM_ENABLE=y` and an `esp_pm_configure()` call
  in [`main.c`](../../../main/main.c) with a 40–160 MHz range. Light sleep stays
  off (see Stage 2).

Collecting a device run is still required to turn the estimates into numbers:
the values logged by the harness must be recorded on a real board.

## Scope and method

Target: ESP32-C3, 8 MB Flash, **no PSRAM**. All dynamic data therefore lives in
internal SRAM, which is the scarce resource.

Plan order is deliberate: **measure first, then remove peak usage, then change
sleep behavior.** Memory fixes are lower risk and also reduce the frequency of
the white-screen restarts that motivated the crash work; power work changes
observable product behavior and must be staged behind measurement.

## Memory budget today

### Statically resident application state

[`app_state.c`](../../../main/app_state.c) holds one `app_runtime_t` singleton.
The largest members, computed from the model headers:

| Member | Size (computed) | Notes |
| --- | --- | --- |
| `routine` | ~13.2 KB | `APP_ROUTINE_WEEKS(2) × APP_ROUTINE_DAYS(7) × 24 nodes × 40 B`. Always resident even when the user has no schedule. |
| `esports` | ~8 KB | 48 match rows + 32 team rows + one nested match detail. |
| `s_vault_blob` | up to `APP_VAULT_CONTAINER_MAX` (~4.7 KB) | Static scratch buffer, not on the stack. |
| `badges`, `reminders`, `totp`, `pomodoro` | a few KB total | 5 badges, 16 reminders, 10 TOTP accounts. |

The two large members are allocated for the lifetime of the application whether
or not those modules are used. This is the main static RAM cost.

### Pipeline buffers and stacks

| Item | Size | Source |
| --- | --- | --- |
| LVGL draw buffer | 240 × 40 × 2 B = **19.2 KB**, single buffer, internal DMA RAM | [`bsp_display_lvgl.c`](../../../components/bsp/src/bsp_display_lvgl.c) |
| LVGL heap pool | **40 KB** static, separate from the system heap | `CONFIG_LV_MEM_SIZE_KILOBYTES=40` (reduced from 48) |
| LVGL port task stack | ~7 KB | `ESP_LVGL_PORT_INIT_CONFIG()` default |
| `app_input` task stack | 8 KB | raised from 4 KB in [`main.c`](../../../main/main.c) |
| Network workers | 4–8 KB each, created per request and expected to exit | [`app_net.c`](../../../main/net/app_net.c) |
| `ui_beep` 3 KB, metronome 4 KB, provisioning 6 KB | transient | various |

### Peak risk: LVGL object count

The system heap and the 40 KB LVGL pool are separate. A page with many objects
can exhaust the pool even when the system heap has room, which is exactly the
"backlight-lit white screen" failure mode; the NULL guards added in
[`ui_theme.c`](../../../main/ui/ui_theme.c) turn that failure from a crash into a
graceful skip, but they do not remove the pressure.

The routine page ([`ui_routine.c`](../../../main/ui/ui_routine.c)) was the worst
case: four tabs, and `show_tab()` built a tab without releasing the contents of
the tabs built before it, so visiting all four kept
`today (24 rows) + week (7) + edit (26) + options (6)` alive at once. Each
`ui_row_t` cost four LVGL objects (container, indicator, title, value), so the
four tabs could hold roughly 250 objects. This is now addressed: `show_tab()`
releases hidden tabs, and rows no longer carry the indicator object. The
resulting peak still needs `lv_mem_monitor()` confirmation on device; the
estimate alone is not a result.

## Memory optimization plan

### Stage 1 — measure (do first) — **harness implemented, device run pending**

The logging described below is implemented in
[`app_metrics.c`](../../../main/app_metrics.c) and wired into the call sites:

- Log `lv_mem_monitor()` (free, fragmentation, max used) after entering and after
  leaving each module page, and after each tab switch in the routine page.
- Log `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` and
  `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` at the same points.
- Log `uxTaskGetStackHighWaterMark()` for `app_input` and the LVGL task on a 60 s
  cadence, and for the network workers when each one exits.
- **Still to do:** record the values reached on a real board during the exact
  action sequences that used to crash: applying/clearing templates and switching
  routine tabs, entering and leaving module pages, repeated OK/UP/DOWN, and after
  provisioning/network use. The harness is the input; the run is the result.

### Stage 2 — remove peak usage (low risk)

1. **Release inactive tabs.** In [`ui_routine.c`](../../../main/ui/ui_routine.c),
   `lv_obj_clean()` the view being hidden in `show_tab()`, and rebuild on show.
   The worst-case object count then collapses to a single tab.
2. **Drop the per-row indicator object.** `ui_row_create()` currently creates a
   child object for the selection bar. A left-side border on the row itself
   (`border_side = LEFT`, 2 px, accent color) renders the same cue with **no
   extra object**, cutting row objects from four to three. Selection state
   becomes a style change on the row.
3. **Rename the row lookup.** `ui_row_set_title_color()` and
   `render_today_rows()` fetch the title with `lv_obj_get_child(row.obj, 1)`,
   which depends on the indicator being child index 1. Removing the indicator
   changes that index; store the title label in `ui_row_t` instead of relying on
   child order.
4. Re-measure. If the peak is still close to the pool limit, repeat steps 1–2 on
   the next-largest page (settings, vault, esports detail).

### Stage 3 — right-size the pool

- The pool has already been reduced 48 → 40 KB ahead of measurement, because the
  system-heap pressure was the observed symptom. Confirm with high-water data
  that 40 KB still clears the real peak plus margin; if not, raise it back, and
  if there is slack, lower it further.
- Alternative: switch LVGL to the system heap (`CONFIG_LV_USE_CLIB_MALLOC`), so
  the pool limit and the system heap are no longer two silos. This trades a
  hard, predictable ceiling for shared pressure and must be re-evaluated against
  heap watermarks; it is **not** recommended as a first step.
- Consider whether the always-resident `routine` (~13 KB) and `esports` (~8 KB)
  budgets can shrink (fewer cached matches, or a smaller node table). These are
  product trade-offs, not free wins.

## Power budget today

- Sleep is **backlight-only**: `sleep_now()` turns the backlight to 0 and sets a
  flag ([`ui_app.c`](../../../main/ui/ui_app.c)). The panel keeps being driven
  and the 1-second `app_tick` keeps running (it must, for reminders, the
  Pomodoro timer, and routine node changes).
- The 1-second tick used to call `ui_app_refresh_status()` every 15 ticks even
  while asleep, invalidating widgets and forcing a redraw of a backlight-off
  panel. **Fixed in Power Stage 1** — the periodic refresh is now skipped while
  asleep and runs once on wake.
- Dynamic frequency scaling is now enabled (Power Stage 2, DFS only): the CPU/APB
  drop to 40 MHz when idle instead of staying at 160 MHz.
- `CONFIG_LV_DEF_REFR_PERIOD=20` makes the LVGL refresh timer wake 50 times per
  second. It does little work when nothing is invalidated, but the wakeups still
  cost CPU time and prevent deep idle; raising it is still an open, measure-first
  item.
- `CONFIG_FREERTOS_HZ=1000` gives a 1 kHz tick.
- Wi-Fi and Bluetooth are opened on demand and released on exit — already a good
  baseline. See the [hardware guide](../../hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md)
  and [softap provisioning notes](../../reference/phoenixzhc/softap-provisioning-and-resource-budget.md).

The backlight dominates power when it is on; panel refresh and CPU dominate when
it is off. The realistic wins are therefore (a) stop redrawing while asleep and
(b) reduce idle CPU wakeups.

## Power optimization plan

### Stage 1 — stop wasted work while asleep (low risk) — **done**

1. In `app_tick`, skip the periodic `ui_app_refresh_status()` while `s_asleep`;
   refresh once on wake. This removes a redraw of an invisible panel every 15 s.
   Implemented in [`ui_app.c`](../../../main/ui/ui_app.c).
2. Ensure nothing else invalidates the screen while asleep. Page `tick()`
   functions are skipped when asleep (kept explicit). The audit found one gap:
   `report_missed_reminders()` opened an alert without waking; it now wakes first,
   matching `check_reminders()`.
3. **Still open (measure first).** Raising `CONFIG_LV_DEF_REFR_PERIOD` from 20 ms
   to 30–40 ms reduces refresh wakeups, but is only acceptable if animated screens
   (identity animation, metronome) still look smooth. Needs an on-device
   before/after comparison; not changed.

### Stage 2 — low-power idle (moderate risk) — **DFS enabled, light sleep deferred**

- **Enabled:** dynamic frequency scaling via `CONFIG_PM_ENABLE=y` and
  `esp_pm_configure()` in [`main.c`](../../../main/main.c) with a 40–160 MHz
  range and `light_sleep_enable = false`.
- **Deferred:** automatic light sleep (`CONFIG_FREERTOS_USE_TICKLESS_IDLE`). It
  changes the USB-Serial/JTAG console, the SPI/DMA path, and the I2S audio path,
  and the plan requires these to be validated on device, not assumed. Turn it on
  only after confirming on a real board: the USB console survives idle, the panel
  refreshes without tearing, and playback/capture do not glitch.
- **Still open:** `CONFIG_FREERTOS_HZ=1000 → 100`. Check every consumer of tick
  timing (button debounce windows, `lv_tick` source, network timeouts) first; do
  not change it speculatively.

### Stage 3 — real sleep (product decision)

- Light sleep with GPIO wake, or deep sleep, would cut idle power far below
  backlight-off. Both are already demonstrated in `demo_low_power` and covered
  by [display refresh and deep sleep notes](../../reference/shinku-chen/display-refresh-and-deep-sleep.md)
  and [deep-sleep peripheral power-off notes](../../reference/shinku-chen/deep-sleep-peripheral-power-off.md).
- The blocker is behavioral: with the CPU asleep or reset, reminders, the
  Pomodoro countdown, routine node alerts, and network all stop. That is a
  product decision, not a technical one. Deep sleep in particular reboots the
  application on wake.
- If light sleep is chosen, it is **not** a drop-in for backlight-off; the wake
  path, peripheral hold, and reminder catch-up all need design.

## Validation

- Memory changes: re-run the crash reproduction sequences and confirm the logged
  LVGL free-memory and heap watermarks stay clear of the limit. Run the full gate
  (`./tools/validate.sh`) and report build and host-test results separately.
- Power changes: measure current draw with a power meter in at least three
  states — backlight on (100% and 30%), and asleep — before and after. Report
  measured mA, not estimates. On-device battery runtime then follows from the
  cell capacity.
- Any sleep change must be validated for wake correctness: every button wakes
  the device and restores the previous screen.

## Open questions

- Which target matters more: the shortest possible battery life gain, or
  preserving background reminders and timing while idle? This decides whether
  Stage 3 is in scope.
- Is reducing the always-resident `routine`/`esports` caches acceptable, or are
  their current limits a product requirement?
- Is a measurement harness (power meter and a repeatable action script)
  available, or should Stage 1 include building one?
