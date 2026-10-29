#include "s3gv.h"
#include <switch.h>
#include "generated/gear_tables.h"
#include <stdio.h>
#include <string.h>

static inline uint16_t rd16(const uint8_t* d, size_t off) {
    return (uint16_t)(d[off] | (d[off+1] << 8));
}
static inline uint32_t rd32(const uint8_t* d, size_t off) {
    return (uint32_t)d[off] | ((uint32_t)d[off+1] << 8) |
           ((uint32_t)d[off+2] << 16) | ((uint32_t)d[off+3] << 24);
}

#define ABIL_INTENSIFY_ACTION 14
#define ABIL_DROP_ROLLER      26

bool gear_validate(const uint8_t* d) {
    if (rd16(d, 0x00) == 0) return false;
    if (d[0x50] > 5) return false;
    if (d[0x68] > d[0x50] + 1) return false;
    if (d[0x54] > ABIL_DROP_ROLLER) return false;
    for (int i = 0; i < 3; i++)
        if (d[0x6C + i*4] > ABIL_INTENSIFY_ACTION) return false;
    if (rd32(d, 0x80) == 0) return false;
    return true;
}

void gear_parse(GearInfo* g, const uint8_t* data, uint8_t gear_type) {
    memcpy(g->raw, data, GEAR_SIZE);
    g->gear_type = gear_type;
    g->id = rd16(data, 0x00);
    g->exp = rd16(data, 0x48);
    g->rarity = data[0x50];
    g->primary = data[0x54];
    g->sub_count = data[0x68];
    g->subs[0] = data[0x6C];
    g->subs[1] = data[0x70];
    g->subs[2] = data[0x74];
    g->seed = rd32(data, 0x80);
    g->name = g->brand = g->rowid = "???";
    const GearDataEntry* e = gear_data_find(gear_type, g->id);
    if (e) { g->name = e->name; g->brand = e->brand; }
}

bool gear_read_list(GearInfo* out, int max, uint64_t base, uint64_t start_pos, uint8_t type,
                    const char** status, int* count) {
    const int CHUNK = 10;
    uint64_t pos = start_pos;
    uint8_t buf[GEAR_SIZE * CHUNK];
    int n = 0;
    bool valid = true;
    while (valid) {
        if (mem_read(base + pos, buf, sizeof(buf)) != 0) {
            *status = "mem read failed";
            return false;
        }
        for (int i = 0; i < CHUNK; i++) {
            const uint8_t* d = buf + i * GEAR_SIZE;
            if (!gear_validate(d)) { valid = false; break; }
            if (n < max) gear_parse(&out[n++], d, type);
        }
        pos += (uint64_t)(GEAR_SIZE * CHUNK);
    }
    *count = n;
    return true;
}

static const char* ABILITY_NAMES[] = {
    "???", "Ink Saver (Main)", "Ink Saver (Sub)", "Ink Recovery Up",
    "Run Speed Up", "Swim Speed Up", "Special Charge Up", "Special Saver",
    "Special Power Up", "Quick Respawn", "Quick Super Jump", "Sub Power Up",
    "Ink Resistance Up", "Sub Resistance Up", "Intensify Action",
    "Opening Gambit", "Last Ditch Effort", "Tenacity", "Comeback",
    "Ninja Squid", "Haunt", "Thermal Ink", "Respawn Punisher",
    "Ability Doubler", "Stealth Jump", "Object Shredder", "Drop Roller",
};

const char* ability_name(int id) {
    if (id >= 0 && id <= 26) return ABILITY_NAMES[id];
    return "???";
}

int gacha_read(uint64_t base, GachaSeed* out, int max) {
    uint8_t block[GACHA_SIZE];
    if (mem_read(base, block, sizeof(block)) != 0) return -1;
    int n = 0;
    for (uint32_t i = 0; i < GACHA_SIZE; i += GACHA_STRUCT_SIZE) {
        const uint8_t* chunk = block + i;
        uint32_t key = rd32(chunk, 0);
        uint32_t type;
        switch (key) {
            case GACHA_KEY_NORMAL: type = 0; break;
            case GACHA_KEY_FEST:   type = 1; break;
            case GACHA_KEY_MURCH:  type = 2; break;
            case GACHA_KEY_TITLE:  type = 3; break;
            case GACHA_KEY_BANNER: type = 4; break;
            default: continue;
        }
        if (n >= max) break;
        out[n].type = type;
        out[n].s0 = rd32(chunk, GACHA_SEED_OFFSET + 0x00);
        out[n].s1 = rd32(chunk, GACHA_SEED_OFFSET + 0x04);
        out[n].s2 = rd32(chunk, GACHA_SEED_OFFSET + 0x08);
        out[n].s3 = rd32(chunk, GACHA_SEED_OFFSET + 0x0C);
        n++;
    }
    return n;
}

// Scan the heap for a run of valid 0xB0-stride gear structs whose IDs resolve
// in this type's name table. Returns the address of the first gear, or -1.
int64_t gear_locate_list(int type, uint64_t start) {
    uint64_t addr = start ? start : 0x8000000ULL;
    static uint8_t buf[0x10000];
    while (addr != 0) {
        uint64_t raddr, rsize;
        uint32_t rtype, rperm;
        if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) return -1;
        if (addr != raddr) addr = raddr;
        bool readable = (rperm & Perm_R) != 0;
        bool is_heap = (rtype == MemType_Heap);
        if (readable && is_heap && rsize >= 0x100000) {
            uint64_t skip = (uint64_t)start > raddr ? (uint64_t)start - raddr : 0;
            if (skip >= rsize) { uint64_t next = raddr + rsize; if (next <= addr) break; addr = next; continue; }
            uint64_t pos = skip;
            while (pos < rsize) {
                size_t n = rsize - pos;
                if (n > sizeof(buf)) n = sizeof(buf);
                if (mem_read(raddr + pos, buf, n) == 0) {
                    for (size_t o = 0; o + 0x2B0 <= n; o++) {
                        if (!gear_validate(buf + o)) continue;
                        if (!gear_validate(buf + o + GEAR_SIZE)) continue;
                        if (!gear_validate(buf + o + 2 * GEAR_SIZE)) continue;
                        uint16_t id0 = buf[o] | (buf[o+1] << 8);
                        uint16_t id1 = buf[o + GEAR_SIZE] | (buf[o + GEAR_SIZE + 1] << 8);
                        if (gear_data_find(type, id0) && gear_data_find(type, id1)) {
                            return (int64_t)(raddr + pos + o);
                        }
                    }
                }
                pos += n;
                if (n == sizeof(buf) && pos < rsize) pos -= 0x2B0; // overlap
            }
        }
        uint64_t next = raddr + rsize;
        if (next <= addr) break;
        addr = next;
    }
    return -1;
}

// Scan the heap for a short "island" run of valid 0xB0-stride gear structs:
// invalid struct immediately before, run of valid structs, then invalid —
// that's the player's owned-gear inventory, not the full item database
// (which is one huge run). IDs must resolve in this type's table.
// Returns the address of the first gear, or -1.
int64_t gear_locate_inventory(int type, int* out_len, uint64_t start) {
    uint64_t addr = start ? start : 0x8000000ULL;
    static uint8_t buf[0x10000];
    if (out_len) *out_len = 0;
    while (addr != 0) {
        uint64_t raddr, rsize;
        uint32_t rtype, rperm;
        if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) return -1;
        if (addr != raddr) addr = raddr;
        bool readable = (rperm & Perm_R) != 0;
        bool is_heap = (rtype == MemType_Heap);
        if (readable && is_heap && rsize >= 0x100000) {
            uint64_t skip = (uint64_t)start > raddr ? (uint64_t)start - raddr : 0;
            if (skip >= rsize) { uint64_t next = raddr + rsize; if (next <= addr) break; addr = next; continue; }
            uint64_t pos = skip;
            while (pos < rsize) {
                size_t n = rsize - pos;
                if (n > sizeof(buf)) n = sizeof(buf);
                if (mem_read(raddr + pos, buf, n) == 0) {
                    for (size_t o = GEAR_SIZE; o + 0x2B0 <= n; o++) {
                        if (!gear_validate(buf + o)) continue;
                        if (gear_validate(buf + o - GEAR_SIZE)) continue; // must be preceded by invalid
                        // extend the run
                        size_t len = 0;
                        while (o + len * GEAR_SIZE + GEAR_SIZE <= n && len < 64 &&
                               gear_validate(buf + o + len * GEAR_SIZE))
                            len++;
                        if (len == 0 || len >= 64) continue;
                        if (o + len * GEAR_SIZE + GEAR_SIZE > n) continue; // run hit chunk end, skip
                        if (gear_validate(buf + o + len * GEAR_SIZE)) continue; // must be followed by invalid
                        uint16_t id0 = buf[o] | (buf[o+1] << 8);
                        uint16_t id1 = buf[o + GEAR_SIZE] | (buf[o + GEAR_SIZE + 1] << 8);
                        if (gear_data_find(type, id0) && gear_data_find(type, id1)) {
                            if (out_len) *out_len = (int)len;
                            return (int64_t)(raddr + pos + o);
                        }
                    }
                }
                pos += n;
                if (n == sizeof(buf) && pos < rsize) pos -= 0x2B0;
            }
        }
        uint64_t next = raddr + rsize;
        if (next <= addr) break;
        addr = next;
    }
    return -1;
}

// Scan all readable non-code regions for a valid gear struct with a specific
// known ID (e.g. a piece the user actually owns), then expand to the full run
// boundaries. The exact-ID requirement makes false positives negligible, so we
// don't restrict to heap regions (the lists may live in game-mapped memory).
// Returns the run start address, sets *out_len. -1 if not found.
int64_t gear_locate_id(int type, uint16_t want, int* out_len) {
    uint64_t addr = 0x8000000ULL;
    static uint8_t buf[0x10000];
    if (out_len) *out_len = 0;
    while (addr != 0) {
        uint64_t raddr, rsize;
        uint32_t rtype, rperm;
        if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) return -1;
        if (addr != raddr) addr = raddr;
        bool readable = (rperm & Perm_R) != 0;
        bool is_code = (rtype == MemType_CodeStatic || rtype == MemType_ModuleCodeStatic);
        if (readable && !is_code && rsize >= 0x100000) {
            uint64_t pos = 0;
            while (pos < rsize) {
                size_t n = rsize - pos;
                if (n > sizeof(buf)) n = sizeof(buf);
                if (mem_read(raddr + pos, buf, n) == 0) { pos += n; continue; }
                for (size_t o = 0; o + GEAR_SIZE <= n; o++) {
                    uint16_t id = buf[o] | (buf[o+1] << 8);
                    if (id != want) continue;
                    if (!gear_validate(buf + o)) continue;
                    uint64_t cand = raddr + pos + o;
                    // Expand backward: previous struct valid?
                    uint64_t run_start = cand;
                    uint8_t prev[GEAR_SIZE];
                    for (int k = 1; k <= 64; k++) {
                        if (mem_read(cand - (uint64_t)k * GEAR_SIZE, prev, GEAR_SIZE) != 0) break;
                        if (!gear_validate(prev)) break;
                        run_start = cand - (uint64_t)k * GEAR_SIZE;
                    }
                    // Expand forward
                    int len = (int)((cand - run_start) / GEAR_SIZE) + 1;
                    uint8_t nxt[GEAR_SIZE];
                    for (;;) {
                        if (mem_read(run_start + (uint64_t)len * GEAR_SIZE, nxt, GEAR_SIZE) != 0) break;
                        if (!gear_validate(nxt)) break;
                        len++;
                        if (len > 256) break;
                    }
                    if (out_len) *out_len = len;
                    return (int64_t)run_start;
                }
                pos += n;
                if (n == sizeof(buf) && pos < rsize) pos -= GEAR_SIZE;
            }
        }
        uint64_t next = raddr + rsize;
        if (next <= addr) break;
        addr = next;
    }
    return -1;
}

// leanny.github.io/splat3seedchecker settings format
bool dump_json(const char* path, const GearInfo* gears, int count) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    const char* names[3] = { "HaveGearHeadMap", "HaveGearClothesMap", "HaveGearShoesMap" };
    fprintf(f, "{\n");
    for (int t = 0; t < 3; t++) {
        fprintf(f, "%s\"%s\": {", t ? ",\n" : "", names[t]);
        bool first = true;
        for (int i = 0; i < count; i++) {
            if (gears[i].gear_type != t) continue;
            uint8_t p = gears[i].primary;
            int main_skill = p - 1;
            if (p - 1 >= ABIL_INTENSIFY_ACTION)
                main_skill = p - ABIL_INTENSIFY_ACTION + 99;
            fprintf(f, "%s\n    \"%u\": { \"MainSkill\": %d, \"RandomContext\": %u, "
                       "\"ExDrinksArray\": [], \"ExSkillArray\": [%d, %d, %d] }",
                    first ? "" : ",", gears[i].id, main_skill, gears[i].seed,
                    gears[i].subs[0] - 1, gears[i].subs[1] - 1, gears[i].subs[2] - 1);
            first = false;
        }
        fprintf(f, "\n  }");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    return true;
}
