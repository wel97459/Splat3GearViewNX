# Splat3GearView NX

Homebrew Switch port of [LegoFigure11/Splat3GearView](https://github.com/LegoFigure11/Splat3GearView) — Splatoon 3 worn-gear reader that reads game memory **directly from the console** (no sys-botbase, no PC required). Shows your currently equipped head/clothes/shoes with abilities and seeds, and exports the leanny seed-checker JSON.

Two builds:

| Build | Path | Use |
|---|---|---|
| **NRO app** | `/switch/Splat3GearViewNX/Splat3GearViewNX.nro` (this repo) | Full framebuffer UI with gear icons; run from hbmenu (game suspended) |
| **Ultrahand overlay** | `/switch/.overlays/Splat3GearViewNX.ovl` (separate repo: `repos/Splat3GearViewNX-ovl`) | Tesla-style menu over the *running* game (ZL+ZR+↓) |

## How it works

- Attaches to the running Splatoon 3 process via `pmdmntGetApplicationProcessId` + `svcDebugActiveProcess` (Atmosphere only), read-only
- Anchor chain, all content-validated (nothing user-specific baked in):
  1. **Gacha block** (RNG keys): fixed address → `heap_base + 0xBA416F70` probe → full scan as last resort
  2. **Loadout triplet**: 3 consecutive `(u32 gearId, u32 flag)` pairs at gacha-region `+0x4F0`, each ID resolved in its type's gear table
  3. **Worn records**: 0xB0 gear structs (`Gear.cs` layout: exp@0x48, rarity@0x50, primary@0x54, sub_count@0x68, subs@0x6C+i*4, seed@0x80) found by a fast window scan around the stable gear-list area, backed by a **dup-group scan** bounded to the gacha's ~24MB heap region
- **Discriminator**: real player records exist as 2+ byte-identical copies (same abilities + seed) at different addresses; junk never duplicates exactly. Candidates are grouped by payload and the highest-scoring duplicate group wins. Game-logic caps enforced (`sub_count ≤ 3`, ability IDs in range, only unlocked slots scored)
- Gear names/brands come from Leanny's JSON, precompiled into C tables at build time (`scripts/gen_tables.py`); 27 ability icons from the upstream repo shipped in romfs
- Rendering is a raw framebuffer (720p RGBA8888) with stb_truetype on the shared system font and stb_image for icons — no SDL

## Usage (NRO)

1. Launch Splatoon 3, then press HOME → open the **Album** (hbmenu) — the game stays loaded (suspended)
2. Run `Splat3GearViewNX.nro`
3. **A** = read worn gear, **X** = export leanny JSON, **+** = exit
4. Copy `dump.json` from SD and upload at https://leanny.github.io/splat3seedchecker/#/settings

Export format matches upstream exactly: `MainSkill` remap (≥ IntensifyAction → `+99`), `RandomContext` = gear seed, `ExSkillArray` = each sub − 1, raw subs for all 3 slots.

## Requirements

- Atmosphere CFW (debug attach needs pm:dmnt access)
- Splatoon 3 — offsets can change every game patch. The anchor chain is content-validated so most offsets self-heal, but after a game patch check `src/s3gv.h` against the upstream repo's `Structures/Offsets.cs`

## Build

```
export DEVKITPRO=/opt/devkitpro DEVKITA64=/opt/devkitpro/devkitA64
make          # NRO (this repo)
```
Requires devkitPro devkitA64 + libnx. The overlay build lives in `repos/Splat3GearViewNX-ovl` and links libtesla/libultra (cloned to `repos/libultrahand`; its download helper is stubbed out so no curl/mbedtls portlibs are needed).

## Offline analysis

`scripts/inventory_scan.py` runs the dup-group scan over raw memory dumps and maps IDs to names/brands, writing leanny-format `inventory.json`. On a real 18MB dump it recovered the full 7-item inventory — the gear list is contiguous 0xB0-strided records, so a future inventory view is feasible.

## Not ported

- Tableturf grind macro — needs virtual controller input, which requires a sysmodule (sys-botbase style). Not possible from an applet NRO or overlay.
- Full inventory browsing UI — records are findable (see above), UI not built.

## Notes

- On-device diagnostics go to `sd:/switch/Splat3GearViewNX/log.txt` — the primary debugging artifact; screen stays clean by design
- Builds are distributed hash-named via the file host (`https://dl.nvme.sh/<sha8>.nro`), never via Telegram attachments (stale-cache risk)
