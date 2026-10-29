#!/usr/bin/env python3
"""Generate C data tables from Leanny's JSON: id -> name, brand per gear type."""
import json, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

def gen(type_key, master_key, json_file):
    master = json.load(open(f'{ROOT}/res/json/EUen.json'))[master_key]
    gear = json.load(open(f'{ROOT}/res/json/{json_file}'))
    brands = json.load(open(f'{ROOT}/res/json/EUen.json'))['CommonMsg/Gear/GearBrandName']
    rows = []
    for g in gear:
        if 'Id' not in g or 'Brand' not in g or '__RowId' not in g:
            continue
        rowid = g['__RowId']
        masterid = rowid.split('_')[1]
        name = master.get(masterid)
        brand = brands.get(g['Brand'])
        icon = f'romfs:/icons/{rowid}.png'
        rows.append((g['Id'], name or rowid, brand or g['Brand'], icon))
    rows.sort(key=lambda r: r[0])
    out = []
    out.append('#include "gear_tables.h"\n')
    out.append(f'// Generated from {json_file} + EUen.json. Do not edit.\n')
    out.append(f'const GearDataEntry g_{type_key}_data[] = {{\n')
    for id, name, brand, icon in rows:
        out.append(f'    {{ {id}, "{esc(name)}", "{esc(brand)}", "{esc(icon)}" }},\n')
    out.append('};\n')
    out.append(f'const size_t g_{type_key}_count = {len(rows)};\n')
    return ''.join(out)

def main():
    os.makedirs(f'{ROOT}/src/generated', exist_ok=True)
    specs = [
        ('head', 'CommonMsg/Gear/GearName_Head', 'GearInfoHead.json'),
        ('clothes', 'CommonMsg/Gear/GearName_Clothes', 'GearInfoClothes.json'),
        ('shoes', 'CommonMsg/Gear/GearName_Shoes', 'GearInfoShoes.json'),
    ]
    c = ''
    for key, master_key, json_file in specs:
        c += gen(key, master_key, json_file)
    open(f'{ROOT}/src/generated/gear_tables.c', 'w').write(c)
    h = '''#ifndef GEAR_TABLES_H
#define GEAR_TABLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint16_t id;
    const char* name;
    const char* brand;
    const char* icon; // romfs path to 128x128 PNG
} GearDataEntry;

extern const GearDataEntry g_head_data[];
extern const size_t g_head_count;
extern const GearDataEntry g_clothes_data[];
extern const size_t g_clothes_count;
extern const GearDataEntry g_shoes_data[];
extern const size_t g_shoes_count;

const GearDataEntry* gear_data_find(int type, uint16_t id);

#endif
'''
    open(f'{ROOT}/src/generated/gear_tables.h', 'w').write(h)

    impl = '''
#include "gear_tables.h"

const GearDataEntry* gear_data_find(int type, uint16_t id) {
    const GearDataEntry* table = NULL;
    size_t count = 0;
    switch (type) {
        case 0: table = g_head_data; count = g_head_count; break;
        case 1: table = g_clothes_data; count = g_clothes_count; break;
        case 2: table = g_shoes_data; count = g_shoes_count; break;
        default: return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        if (table[i].id == id) return &table[i];
    }
    return NULL;
}
'''
    open(f'{ROOT}/src/generated/gear_tables.c', 'a').write(impl)

if __name__ == '__main__':
    main()
