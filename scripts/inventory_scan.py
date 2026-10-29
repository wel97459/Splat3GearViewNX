#!/usr/bin/env python3
"""Offline inventory scan over raw memory dumps.

Reads one or more gearlist.bin dumps (raw heap windows from the NRO's Y
button), finds every byte-offset that looks like a valid 0xB0 gear struct
(same layout the PC tool uses: id@0, exp@0x48, rarity@0x50, primary@0x54,
sub_count@0x68, subs@0x6C+i*4, seed@0x80), collects signatures, and reports
DUPLICATED signatures (2+ identical copies) mapped to gear names — the same
duplicate-group discriminator that cracked the worn-item read on-device.

Usage: python3 scripts/inventory_scan.py dump1.bin [dump2.bin ...]
"""
import struct, sys, os, re, json

GEAR_SIZE = 0xB0
ABIL = ["???", "Ink Saver (Main)", "Ink Saver (Sub)", "Ink Recovery Up",
        "Run Speed Up", "Swim Speed Up", "Special Charge Up", "Special Saver",
        "Special Power Up", "Quick Respawn", "Quick Super Jump", "Sub Power Up",
        "Ink Resistance Up", "Sub Resistance Up", "Intensify Action",
        "Opening Gambit", "Last Ditch Effort", "Tenacity", "Comeback",
        "Ninja Squid", "Haunt", "Thermal Ink", "Respawn Punisher",
        "Ability Doubler", "Stealth Jump", "Object Shredder", "Drop Roller"]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def load_tables():
    """Parse src/generated/gear_tables.c -> {type: {id: (name, brand)}}"""
    src = open(f'{ROOT}/src/generated/gear_tables.c').read()
    tables = [dict() for _ in range(3)]
    cur = -1
    for line in src.splitlines():
        m = re.search(r'GearDataHead|GearDataClothes|GearDataShoes', line)
        if m:
            cur = 0 if 'Head' in m.group(0) else 1 if 'Clothes' in m.group(0) else 2
        m = re.match(r'\s*\{\s*(\d+),\s*"([^"]*)",\s*"([^"]*)"', line)
        if m and cur >= 0:
            tables[cur][int(m.group(1))] = (m.group(2), m.group(3))
    return tables

def scan(data, tables):
    sigs = {}
    n = len(data)
    for off in range(0, n - GEAR_SIZE, 4):
        d = data[off:off + GEAR_SIZE]
        gid = struct.unpack_from('<H', d, 0)[0]
        if gid == 0 or gid > 0xFFFF:
            continue
        rarity = d[0x50]
        if rarity > 5:
            continue
        cnt = d[0x68]
        if cnt > 3:
            continue
        prim = d[0x54]
        if prim < 1 or prim > 26:
            continue
        subs = [d[0x6C + i * 4] for i in range(3)]
        if any(v < 1 or v > 14 for v in subs[:cnt]):
            continue
        seed = struct.unpack_from('<I', d, 0x80)[0]
        if seed == 0:
            continue
        # type-resolve: which table has this id?
        tname = None
        for t in range(3):
            if gid in tables[t]:
                tname = (t, gid)
                break
        if not tname:
            continue
        sig = (tname[0], gid, rarity, prim, cnt, subs[0], subs[1], subs[2], seed)
        sigs.setdefault(sig, []).append(off)
    return sigs

def main():
    tables = load_tables()
    all_sigs = {}
    for path in sys.argv[1:]:
        data = open(path, 'rb').read()
        print(f"== {os.path.basename(path)} ({len(data)//1024} KB) ==")
        sigs = scan(data, tables)
        for sig, offs in sigs.items():
            all_sigs.setdefault(sig, []).extend((os.path.basename(path), o) for o in offs)
    print("\n== duplicated signatures (2+ identical copies) ==")
    groups = sorted(((sig, offs) for sig, offs in all_sigs.items() if len(offs) >= 2),
                    key=lambda x: (-len(x[1]), x[0][0], x[0][1]))
    for (t, gid, rar, prim, cnt, s0, s1, s2, seed), offs in groups:
        name, brand = tables[t].get(gid, ("?", "?"))
        subs = ", ".join(ABIL[v] if v <= 26 else f"raw{v}" for v in (s0, s1, s2)[:cnt]) or "-"
        print(f"[{'HC S'[t]}] {name} ({brand})  rar {rar}  main {ABIL[prim]}"
              f"  subs: {subs}  seed {seed:08X}  x{len(offs)} @ {[hex(o) for o in offs[:4]]}")
    print(f"\n{len(groups)} duplicated groups total")
    # also: dump full JSON of duplicated groups for the seed checker
    out = {"HaveGearHeadMap": {}, "HaveGearClothesMap": {}, "HaveGearShoesMap": {}}
    keymap = ["HaveGearHeadMap", "HaveGearClothesMap", "HaveGearShoesMap"]
    for (t, gid, rar, prim, cnt, s0, s1, s2, seed), offs in groups:
        main_skill = (prim - 14) + 99 if prim - 1 >= 14 else prim - 1
        out[keymap[t]][str(gid)] = {
            "MainSkill": main_skill, "RandomContext": seed,
            "ExDrinksArray": [], "ExSkillArray": [s0 - 1, s1 - 1, s2 - 1]}
    dst = os.path.join(os.path.dirname(sys.argv[1]) or '.', 'inventory.json')
    json.dump(out, open(dst, 'w'), indent=1)
    print(f"leanny-format JSON written to {dst}")

if __name__ == '__main__':
    main()
