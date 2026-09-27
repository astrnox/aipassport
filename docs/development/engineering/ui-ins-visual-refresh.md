<p align="right">
  <a href="ui-ins-visual-refresh.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Instagram-style UI/UX Refresh Plan

Status: **planning only**. This document proposes a visual refresh and does not
change any runtime behavior by itself. Nothing here should be implemented until
the open questions at the end are settled.

## Scope

This plan covers the visual language of the Passport toolbox application under
`main/ui/`. Color and typography already have a single source of truth —
[`ui_theme.c`](../../../main/ui/ui_theme.c) — so the refresh is mostly a token
change plus a component audit. Navigation, page structure, data model, and
button behavior are out of scope.

## Why the current theme reads "AI-generated"

The palette in [`ui_theme.c`](../../../main/ui/ui_theme.c) is coherent but lands
in the default look of machine-generated dashboards:

- The dark palette pairs a near-black navy background (`0x0B0E13`), blue-grey
  surfaces (`0x151A22`, `0x1F2833`) and an electric cyan accent (`0x22D3EE`).
  Cyan-on-navy is the single most common "AI product" signature.
- Several semantic colors are highly saturated (`live 0xFF4757`,
  `ok 0x34D399`, `soon 0xFFB020`), which reads as status LEDs rather than a
  calm editorial interface.
- The light accent (`0x0891B2`) continues the same cool blue family.

What is already good and should be preserved: no gradients, no drop shadows, and
hierarchy expressed through 1px borders, a 2px semantic bar, and surface
brightness. The refresh keeps that discipline and only changes the palette mood
and a few component details.

## Target direction

"Instagram-style" here means **editorial minimalism**, not a copy of any product:

- Warm neutral paper tones (off-white / warm charcoal) instead of blue-grey.
- A single, muted accent hue; everything else is greyscale.
- Generous spacing and a small set of radii (4 / 6 / 8) instead of visual noise.
- Hierarchy from spacing, opacity, and 1px lines — never from gradient or glow.

### Forbidden

- Any gradient (the current code has none; keep it that way).
- Blue-violet hues and any accent in the cyan-to-purple band (roughly 200°–280°).
- More than one accent hue at a time.
- Fully saturated banner fills; banners become neutral panels with an accent
  edge instead.

## Palette

Only `PALETTE_DARK` and `PALETTE_LIGHT` change. Every page keeps calling the
existing `ui_c_*()` accessors, so no page code is touched.

The values below are a **starting proposal**, chosen to be verified on the real
RGB565 panel. Contrast — especially `dim` text on `card` and `accent` on `card` —
must be checked per screen on device before this is accepted.

| Token | Role | Dark proposal | Light proposal |
| --- | --- | --- | --- |
| `bg` | page background | `0x100E0C` | `0xF7F4F0` |
| `card` | card / list row | `0x1B1815` | `0xFFFFFF` |
| `panel` | nested container | `0x25201B` | `0xEFEAE3` |
| `text` | primary text | `0xF3EEE8` | `0x1B1714` |
| `dim` | secondary text | `0xA79E95` | `0x6E655C` |
| `accent` | the single accent | `0xE08B5A` | `0xC0623A` |
| `live` | in-progress / live | `0xE2604A` | `0xB93A2A` |
| `soon` | upcoming | `0xD6A24C` | `0x9A6B1E` |
| `done` | completed | `0x6F6862` | `0xA69E95` |
| `ok` | success / online | `0x7FA37A` | `0x567B50` |
| `warn` | warning / uncalibrated | `0xD98E3F` | `0x9A6B1E` |
| `sel` | selected row background | `0x2A231E` | `0xE9E2D9` |
| `border` | 1px separators | `0x2E2822` | `0xDDD5CB` |

Design rationale: the accent is a warm terracotta/amber that stays distinct from
the retained `live` red and `warn` amber used for in-progress and warnings, so
status meaning does not collapse into the accent.

## Typography

- Keep `app_font_12/16/20` (CJK) and Montserrat 32/40 (numerals only). No new
  font assets are needed.
- Do not introduce a bold/medium CJK weight; the generated fonts have a single
  weight. Build hierarchy from size, color, and spacing instead.
- The numeral-only rule for display sizes stays: it protects Flash and avoids
  shipping large CJK glyph sets.

## Components

| Component | Change |
| --- | --- |
| Cards (`ui_card_create`) | Keep 1px border and radius 8. Replace the 3px left accent bar with a 2px one, and reserve it for status emphasis rather than decoration. |
| Rows (`ui_row_create`) | Keep the 40px height and 6px radius. Move selection emphasis to the `sel` fill; keep the left indicator hidden unless the row is selected. |
| Tabs (`ui_tabs_create`) | Keep the 2px underline, tint it with `accent`. |
| Banners (`ui_banner_create`) | Stop filling with a saturated semantic color. Use `panel` background plus a 3px `accent` (or semantic) left edge, keeping text in `text`. |
| Empty states (`ui_empty_create`) | Add vertical breathing room; the copy stays. |
| Status bar | Keep monochrome; only the LIVE badge may use `live`. |

## Motion

No new animations. Page builds and rebuilds stay on `LV_ANIM_OFF`; the panel
uses a single DMA buffer with no PSRAM, so large re-draws are the frame budget's
main risk. See [meter-ui-smoothing-and-layout](../../reference/y2lin/meter-ui-smoothing-and-layout.md).

## Migration steps

1. Edit the two palettes in [`ui_theme.c`](../../../main/ui/ui_theme.c).
2. Audit remaining hardcoded colors. Only pure-black overlays
   (`ui_home.c`, `ui_settings.c`, `ui_timeedit.c`, `ui_theme.c`) and the
   black/white QR canvas palette (`ui_identity.c`) should remain; everything else
   must come from `ui_c_*()`.
3. Verify contrast per screen on device and capture evidence with the
   [serial screenshot protocol](../../reference/y2lin/serial-screenshot-protocol.md).
4. Check the brand reference for alignment: [brand and product](../../brand/brand-and-product.md).

## Open questions

- **Accent hue.** Terracotta/amber is proposed. Sage green or pure greyscale are
  alternatives; pure greyscale would force a rethink of the `live`/`ok`/`warn`
  semantic colors.
- **Light-theme character.** The proposal keeps a warm off-white. Whether the
  device default should remain dark (as today) is a product decision.
- **Scope.** Token-only swap, or also re-lay-out specific screens (home grid,
  240×320 density)? The proposal assumes token-only for the first pass.
- **Contrast target.** Which minimum contrast ratio is required on the
  transmissive LCD under backlight — a strict WCAG AA target or a looser,
  hand-tuned target.
