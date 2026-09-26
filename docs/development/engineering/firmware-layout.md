<p align="right">
  <a href="firmware-layout.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Firmware Layout

This repository targets an ESP32-C3 with 8 MB Flash. The upstream template ships
a minimal three-partition table; this product has expanded it to hold persisted
user data and animation resources. The layout below is the current, authoritative
one - it does not reserve OTA slots.

## Current layout

The upstream template defaults to a minimal table (24 KB NVS, 4 KB PHY data,
and one factory application covering the rest of Flash). This product
deliberately replaces that layout because the tool box persists far more
per-device data and stores animation frames outside NVS:

| Partition | Type/subtype | Offset | Size | Purpose |
| --- | --- | ---: | ---: | --- |
| `nvs` | data/NVS | `0x9000` | `0x16000` | ESP-IDF and application key-value storage (88 KB) |
| `phy_init` | data/PHY | `0x1F000` | `0x1000` | PHY initialization data |
| `factory` | app/factory | `0x20000` | `0x540000` | The single application image (5.25 MB) |
| `assets` | `0x40`/`0x00` | `0x560000` | `0x2A0000` | Personal-card animation frames (6 slots, 2.625 MB) |

Why each choice:

- **88 KB NVS.** Badge nicknames with up to three QR codes each, vault entries,
  up to ten TOTP accounts, reminders, the routine table, and settings all live
  in NVS. 24 KB left no room for wear levelling headroom and would have started
  failing writes once the vault grew.
- **5.25 MB application.** The three generated Chinese fonts plus LVGL, Wi-Fi, and
  BLE need roughly 2.5-3 MB. 5.25 MB keeps a comfortable margin for growth while
  still leaving room for resources.
- **`assets` is a custom partition type, not `spiffs`/`fat`.** ESP-IDF reserves
  types `0x40`-`0xFE` for application-defined formats; the bootloader ignores
  them. This firmware runs no file system on the partition - the on-flash layout
  is a fixed slot table implemented in `main/app_assets.c` and validated by the
  host tests for `main/logic/app_anim.c`. Borrowing a `spiffs` subtype without a
  file system would have been misleading.
- **Six 448 KB slots, and the partition ends exactly at 8 MB.** A slot must hold
  the largest permitted animation, 96 x 96 RGB565 x 24 frames, which is 442,368
  bytes plus a 48-byte header. Slot offsets are fixed multiples of the slot size
  because a personal card stores only a slot number: if slot size floated with
  the partition size, a different partition table would silently repoint already
  stored animations. The last slot therefore ends on the 8 MB boundary, and the
  application partition absorbs the remainder.

The layout still has no OTA slots. Changing it again is allowed; then re-verify
with `./tools/validate.sh --firmware`, which regenerates the merged image and
re-reads the real offsets from `flash_args`.

## Custom layouts

Users may edit `partitions.csv` to resize, move, add, or remove partitions for
their application. A custom table may use OTA slots, filesystem/resource
partitions, or other application-specific data. Keep the 8 MB device boundary,
avoid overlaps, and make sure the application image is flashed at the start of
an app partition large enough to contain it. When a derivative changes its
layout, update that project's documentation and flashing instructions.

## Enforced validation

Run:

```bash
./tools/validate.sh --firmware
```

The check builds in an isolated directory, creates the merged image, reads the
configured image offsets from `flash_args`, validates the partition-table MD5,
partition bounds, unique labels, and non-overlap, then ensures the application
offset matches an app partition large enough to contain it. It intentionally
does not require the default partition list. CI runs the same gate.

Every image listed in `flash_args`, including user-defined resources and OTA
data, must exist, be nonempty and match the merged bytes at its configured
offset. Image ranges must stay within 8 MB and must not overlap. Additional
images must fit entirely inside a configured partition; an offset inside that
partition is allowed. Merely declaring a resource partition does not require a
preloaded image, but listing an image in `flash_args` makes it mandatory.

Upload only `build/FoloToy-AI-Passport-full.bin`; the similarly named app-only
`build/FoloToy-AI-Passport.bin` does not contain the bootloader or partition
table.

## Flashing and stored data

> **No backup of the firmware already installed on the device is required
> before downloading (flashing) new firmware.** Do not make reading out the
> original firmware or saving a full-Flash dump a prerequisite for this
> workflow. The new firmware replaces the original firmware; this workflow
> does not retain an automatic rollback copy or promise that the original
> firmware can be restored.

Firmware and user data are different. If existing NVS settings, application
records, or files must be kept, export or otherwise save them before flashing
using a method supported by that application. Not requiring an original-firmware
backup does not guarantee data preservation or authorize a full-chip erase.

The verified merged image is written from `0x0`. Because the merged file pads
the gaps between images, flashing it can reset the NVS and PHY data regions.
Use the merged image for blank-device provisioning or an intentional complete
refresh. During normal development, use segmented `idf.py flash` when existing
NVS state should be preserved; this also requires a compatible partition layout
and flash targets that do not overwrite those data regions. `idf.py erase-flash`
erases all user data. Do not add it as a routine prerequisite: use it only when
a complete erase is explicitly intended and any data that must be kept has
been saved.
