# Splat3GearView NX

Homebrew Switch port of [LegoFigure11/Splat3GearView](https://github.com/LegoFigure11/Splat3GearView) — Splatoon 3 gear seed checker that reads game memory **directly from the console** (no sys-botbase, no PC required).

## How it works
- Attaches to the running Splatoon 3 process via `pmdmntGetApplicationProcessId` + `svcDebugActiveProcess` (Atmosphere only) and reads RAM read-only
- Same fixed heap offsets as the original tool (v11.3.0): headgear `0xBA24BDD8`, `+0x2C020` per type, gacha block `0xBA342D28`
- Gear names/brands come from Leanny's JSON, precompiled into C tables at build time (`scripts/gen_tables.py`)
- Dumps owned gear in leanny seed-checker JSON format to `sd:/switch/Splat3GearViewNX/dump.json`

## Usage
1. Launch Splatoon 3, then press HOME → open the **Album** (hbmenu) — the game stays loaded (suspended) in memory
2. Run `Splat3GearViewNX.nro`
3. **A** = read gear, **L/R** = browse, **X** = dump leanny JSON to SD, **+** = exit
4. Copy `dump.json` from SD and upload at https://leanny.github.io/splat3seedchecker/#/settings

## Requirements
- Atmosphere CFW (debug attach needs pm:dmnt access)
- Splatoon 3 game version matching the offsets in `src/s3gv.h` (currently v11.3.0 — offsets change every game patch; update from the upstream repo's `Structures/Offsets.cs`)

## Build
```
export DEVKITPRO=/opt/devkitpro
make
```
Requires devkitPro devkitA64 + libnx.

## Not ported (yet)
- Tableturf grind macro — needs virtual controller input, which requires a sysmodule (sys-botbase style). Not possible from an applet NRO.
- Gear/skill icons and SDL graphics — v1 is a console UI; upgrade path is a Tesla overlay.
