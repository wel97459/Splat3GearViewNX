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
    uint32_t p32 = rd32(data, 0x54);
    g->primary = (p32 <= 26) ? p32 : data[0x54];
    g->sub_count = data[0x68];
    for (int i = 0; i < 3; i++) {
        uint32_t s32 = rd32(data, 0x6C + i * 4);
        uint8_t v = (s32 <= 26) ? (uint8_t)s32 : data[0x6C + i * 4];
        // only report slots the game has actually unlocked
        g->subs[i] = (i < g->sub_count) ? v : 0xFF; // 0xFF = empty slot
    }
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
    if (id == 0xFF) return "(empty)";
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

// Scan the heap for a short "island" run of valid 0xB0-stride gear structs:
// invalid struct immediately before, run of valid structs, then invalid —
// that's the player's owned-gear inventory, not the full item database
// (which is one huge run). IDs must resolve in this type's table.
// Returns the address of the first gear, or -1.

// Scan all readable non-code regions for a valid gear struct with a specific
// known ID (e.g. a piece the user actually owns), then expand to the full run
// boundaries. The exact-ID requirement makes false positives negligible, so we
// don't restrict to heap regions (the lists may live in game-mapped memory).
// Returns the run start address, sets *out_len. -1 if not found.

// leanny.github.io/splat3seedchecker settings format
// leanny.github.io/splat3seedchecker settings JSON (matches upstream
// WebsiteFormat/MainWindow.cs): MainSkill is remapped for abilities >=
// IntensifyAction; ExSkillArray = sub ability - 1; subs are the RAW stored
// values (upstream exports all 3 slots regardless of unlock state).
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
            const uint8_t* d = gears[i].raw;
            uint8_t p = d[0x54];
            int main_skill;
            if (p - 1 >= ABIL_INTENSIFY_ACTION)
                main_skill = (p - ABIL_INTENSIFY_ACTION) + 99;
            else
                main_skill = p - 1;
            int s1 = d[0x6C] - 1, s2 = d[0x70] - 1, s3 = d[0x74] - 1;
            fprintf(f, "%s\n    \"%u\": { \"MainSkill\": %d, \"RandomContext\": %u, "
                       "\"ExDrinksArray\": [], \"ExSkillArray\": [%d, %d, %d] }",
                    first ? "" : ",", gears[i].id, main_skill, gears[i].seed, s1, s2, s3);
            first = false;
        }
        fprintf(f, "\n  }");
    }
    fprintf(f, "\n}\n");
    fclose(f);
    return true;
}

// Fast heap-only search for a single gear's full 0xB0 record by ID.
// The owned-gear structs live inside the svc heap (confirmed by logs), so we
// skip the huge non-heap regions that made the full hunt slow.
bool gear_find_struct(uint8_t type, uint16_t id, GearInfo* out) {
    uint64_t addr = 0x8000000ULL;
    static uint8_t buf[0x40000];
    int found = 0;
    int best_score = -1;
    uint64_t best_addr = 0;
    GearInfo best_gi;
    while (addr != 0) {
        uint64_t raddr, rsize;
        uint32_t rtype, rperm;
        if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) break;
        if (addr != raddr) addr = raddr;
        bool readable = (rperm & Perm_R) != 0;
        bool is_heap = (rtype == MemType_Heap);
        if (readable && is_heap && rsize >= GEAR_SIZE) {
            uint64_t pos = 0;
            while (pos < rsize) {
                size_t n = rsize - pos;
                if (n > sizeof(buf)) n = sizeof(buf);
                if (mem_read(raddr + pos, buf, n) == 0) {
                    for (size_t o = 0; o + GEAR_SIZE <= n; o++) {
                        uint16_t gid = buf[o] | (buf[o+1] << 8);
                        if (gid != id) continue;
                        if (!gear_validate(buf + o)) continue;
                        uint8_t prim = buf[o + 0x54];
                        uint8_t scnt = buf[o + 0x68];
                        // score: real worn gear has a rolled primary and unlocked subs;
                        // default/db copies have primary 0 and sub_count 0
                        int score = scnt * 4 + (prim != 0 ? 2 : 0);
                        bool subs_ok = prim != 0;
                        for (int i = 0; i < scnt; i++)
                            if (buf[o + 0x6C + i * 4] < 1) subs_ok = false;
                        if (subs_ok) score += 2;
                        found++;
                        app_log("cand[%d] id %u @%llX+0x%llX: prim %u cnt %u subs %u/%u/%u seed %08X score %d",
                                found, id, (unsigned long long)(raddr + pos + o),
                                (unsigned long long)o, prim, scnt,
                                buf[o + 0x6C], buf[o + 0x70], buf[o + 0x74],
                                rd32(buf + o, 0x80), score);
                        if (score > best_score) {
                            best_score = score;
                            best_addr = raddr + pos + o;
                            gear_parse(&best_gi, buf + o, type);
                        }
                    }
                    pos += n;
                    if (n == sizeof(buf) && pos < rsize) pos -= GEAR_SIZE;
                } else {
                    pos += n;
                }
            }
        }
        uint64_t next = raddr + rsize;
        if (next <= addr) break;
        addr = next;
    }
    app_log("gear_find_struct type %u id %u: %d candidates, best score %d @%llX",
            type, id, found, best_score, (unsigned long long)best_addr);
    if (best_score < 0) return false;
    *out = best_gi;
    return true;
}
