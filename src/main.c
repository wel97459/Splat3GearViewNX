#include "s3gv.h"
#include "generated/gear_tables.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define NRO_PATH_OUT "sdmc:/switch/Splat3GearViewNX/dump.json"
#define LOG_PATH "sdmc:/switch/Splat3GearViewNX/log.txt"

static uint64_t g_gacha_addr = 0;   // gacha block address found this read
static uint64_t g_worn_addr[3] = {0, 0, 0}; // addresses of found worn records
uint64_t g_gear_found_addr = 0; // set by gear_find_struct
uint16_t g_loadout[3] = {0, 0, 0}; // worn gear IDs (head/clothes/shoes), 0 = unknown
GearInfo g_worn_info[3];          // full parsed records for the worn items (seed etc.)
static char g_status[128] = "Press A to read gear from memory";
static bool g_attached = false;


void app_log(const char* fmt, ...) {
    FILE* f = fopen(LOG_PATH, "a");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void show_gear(void) {
    ui_draw(g_status);
}


// A: read the 3 worn items. The equipped loadout lives near the gacha block as
// three consecutive (u32 gearId, u32 flag) pairs - head, clothes, shoes.
static void read_loadout(void) {
    app_log("---- loadout start ----");
    if (mem_attach() != 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach. Game must be running (suspended) behind hbmenu.");
        return;
    }
    g_attached = true;
    // Resolve gacha anchor
    uint64_t gacha = 0;
    GachaSeed seeds[8];
    int nseeds = gacha_read(GACHA_BLOCK, seeds, 8);
    if (nseeds > 0) gacha = GACHA_BLOCK;
    if (!gacha) {
        nseeds = gacha_read(GACHA_BLOCK + DLC_OFFS, seeds, 8);
        if (nseeds > 0) gacha = GACHA_BLOCK + DLC_OFFS;
    }
    if (!gacha) {
        // fast: anchor sits at heap_base + 0xBA416F70-ish on every observed
        // boot (drift <= 0x80). Probe that small window before a full scan.
        uint64_t hb = mem_heap_base();
        if (hb) {
            static const uint64_t inheap[2] = { 0xBA416F70ULL, 0xBA416EF0ULL };
            for (int i = 0; i < 2 && !gacha; i++)
                for (int64_t d = -0x200; d <= 0x200 && !gacha; d += 0x80) {
                    GachaSeed s2[8];
                    if (gacha_read(hb + inheap[i] + d, s2, 8) > 0)
                        gacha = hb + inheap[i] + d;
                }
            if (gacha) app_log("gacha via heap_base+%llX", (unsigned long long)(gacha - hb));
        }
    }
    if (!gacha) {
        int64_t found = mem_locate_gacha();
        if (found < 0) {
            snprintf(g_status, sizeof(g_status), "ERROR: gacha block not found");
            show_gear();
            if (g_attached) mem_detach();
            return;
        }
        gacha = (uint64_t)found;
    }
    g_gacha_addr = gacha;
    nseeds = gacha_read(gacha, seeds, 8);
    app_log("gacha anchor @%llX (seeds %d)", (unsigned long long)gacha, nseeds);

    // The loadout sits in the gacha's (small) heap region, near its start.
    uint64_t raddr = 0, rsize = 0; uint32_t rt = 0, rp = 0;
    mem_query(gacha, &raddr, &rsize, &rt, &rp);
    app_log("gacha region %llX sz %llX t%u", (unsigned long long)raddr, (unsigned long long)rsize, rt);
    static uint8_t buf[0x40000];
    uint64_t found = 0;
    if (raddr && rsize) {
        uint64_t pos = 0;
        while (pos < rsize && !found) {
            size_t n = rsize - pos;
            if (n > sizeof(buf)) n = sizeof(buf);
            if (mem_read(raddr + pos, buf, n) != 0) { pos += n; continue; }
            for (size_t o = 0; o + 24 <= n; o += 4) {
                uint32_t a = buf[o] | (buf[o+1]<<8) | (buf[o+2]<<16) | (buf[o+3]<<24);
                uint32_t b = buf[o+8] | (buf[o+9]<<8) | (buf[o+10]<<16) | (buf[o+11]<<24);
                uint32_t c = buf[o+16] | (buf[o+17]<<8) | (buf[o+18]<<16) | (buf[o+19]<<24);
                if (!a || !b || !c) continue;
                if (a > 0xFFFF || b > 0xFFFF || c > 0xFFFF) continue;
                uint32_t f1 = buf[o+4] | (buf[o+5]<<8) | (buf[o+6]<<16) | (buf[o+7]<<24);
                uint32_t f2 = buf[o+12] | (buf[o+13]<<8) | (buf[o+14]<<16) | (buf[o+15]<<24);
                uint32_t f3 = buf[o+20] | (buf[o+21]<<8) | (buf[o+22]<<16) | (buf[o+23]<<24);
                if (f1 > 1 || f2 > 1 || f3 > 1) continue;
                if (!gear_data_find(0, a) || !gear_data_find(1, b) || !gear_data_find(2, c)) continue;
                found = raddr + pos + o;
                g_loadout[0] = a; g_loadout[1] = b; g_loadout[2] = c;
                app_log("loadout @%llX (region+%llX, gacha%+lld): %u %u %u",
                        (unsigned long long)found, (unsigned long long)(found - raddr),
                        (long long)(found - gacha), a, b, c);
                break;
            }
            pos += n;
            if (n == sizeof(buf) && pos < rsize) pos -= 24;
        }
    }
    if (!found) {
        app_log("loadout pattern not found in gacha region");
        snprintf(g_status, sizeof(g_status), "Worn gear not found in gacha region");
    } else {
        // FAST PATH: worn records live in the gear list as contiguous 0xB0
        // records near stable offsets relative to the gacha region start
        // (clothes ~ region+0x2EA190, head ~ -0x2C020, shoes ~ +0x2C020).
        // The exact slot drifts in 0xB0 steps as the inventory changes, so
        // scan small windows around those anchors instead of fixed reads.
        for (int t = 0; t < 3; t++) memset(&g_worn_info[t], 0, sizeof(GearInfo));
        uint64_t rbase = found - 0x4F0; // triplet is at region+0x4F0
        uint8_t rec[GEAR_SIZE];
        uint64_t win_addr = rbase + 0x2B0000;
        uint32_t win_len = 0x46000;     // covers head+clothes array area
        static uint8_t win[0x46000];
        if (mem_read(win_addr, win, win_len) == 0) {
            for (int t = 0; t < 3; t++) {
                for (uint32_t o = 0; o + GEAR_SIZE <= win_len; o += 4) {
                    uint16_t gid = win[o] | (win[o + 1] << 8);
                    if (gid != g_loadout[t] || !gear_validate(win + o)) continue;
                    if (win[o + 0x54] < 1 || win[o + 0x68] > 3) continue;
                    gear_parse(&g_worn_info[t], win + o, t);
                    g_worn_addr[t] = win_addr + o;
                    app_log("worn[%d] fast @%llX (region+%llX): seed %08X rar %u main %u sub %u/%u/%u",
                            t, (unsigned long long)(win_addr + o),
                            (unsigned long long)(win_addr + o - rbase),
                            g_worn_info[t].seed, g_worn_info[t].rarity,
                            g_worn_info[t].primary, g_worn_info[t].subs[0],
                            g_worn_info[t].subs[1], g_worn_info[t].subs[2]);
                    break;
                }
            }
        } else {
            app_log("fast window read failed @%llX", (unsigned long long)win_addr);
        }
        // shoes window (further along the list)
        if (!g_worn_info[2].id) {
            uint64_t sbase = rbase + 0x314000;
            static uint8_t swin[0x8000];
            if (mem_read(sbase, swin, sizeof swin) == 0) {
                for (uint32_t o = 0; o + GEAR_SIZE <= sizeof swin; o += 4) {
                    uint16_t gid = swin[o] | (swin[o + 1] << 8);
                    if (gid != g_loadout[2] || !gear_validate(swin + o)) continue;
                    if (swin[o + 0x54] < 1 || swin[o + 0x68] > 3) continue;
                    gear_parse(&g_worn_info[2], swin + o, 2);
                    g_worn_addr[2] = sbase + o;
                    app_log("worn[2] fast @%llX (region+%llX): seed %08X rar %u main %u sub %u/%u/%u",
                            (unsigned long long)(sbase + o),
                            (unsigned long long)(sbase + o - rbase),
                            g_worn_info[2].seed, g_worn_info[2].rarity,
                            g_worn_info[2].primary, g_worn_info[2].subs[0],
                            g_worn_info[2].subs[1], g_worn_info[2].subs[2]);
                    break;
                }
            }
        }
        // The loadout record carries a pointer array just BEFORE the triplet
        // (dump: pointers at triplet-0x50..triplet-0x08). Those point at the
        // real gear objects elsewhere in the heap — dereference them and read
        // the full 0xB0 record at the target, matched by ID.
        int nmissing = 0;
        for (int t = 0; t < 3; t++) if (g_loadout[t] && g_worn_info[t].id == 0) nmissing++;
        app_log("fast path done, %d missing", nmissing);
        uint64_t ptrs[12]; int nptr = 0;
        uint8_t pb[0x60];
        uint64_t pscan = found - 0x60;
        if (nmissing && mem_read(pscan, pb, sizeof pb) == 0) {
            for (int i = 0; i + 8 <= (int)sizeof pb; i += 8) {
                uint64_t p = 0;
                for (int k = 7; k >= 0; k--) p = (p << 8) | pb[i + k];
                if (p > 0x1000000000ULL && p < 0x800000000000ULL && nptr < 12)
                    ptrs[nptr++] = p;
            }
        }
        app_log("record pointers: %d found before loadout @%llX", nptr, (unsigned long long)pscan);
        for (int t = 0; t < 3; t++) memset(&g_worn_info[t], 0, sizeof(GearInfo));
        for (int i = 0; i < nptr; i++) {
            for (int soff = 0; soff <= 0x10; soff += 0x10) {
                uint64_t target = ptrs[i] + soff;
                uint64_t ta, ts; uint32_t tt, tp;
                if (mem_query(target, &ta, &ts, &tt, &tp) != 0 || !(tp & Perm_R)) {
                    if (soff == 0) app_log("ptr[%d] %llX unmapped", i, (unsigned long long)ptrs[i]);
                    continue;
                }
                uint8_t gb[0x20];
                if (mem_read(target, gb, sizeof gb) != 0) continue;
                char hex[3 * 0x20 + 8] = "";
                for (int k = 0; k < 0x20; k++)
                    snprintf(hex + k * 3, 4, "%02X ", gb[k]);
                app_log("ptr[%d] %llX+0x%X t%u: %s", i, (unsigned long long)ptrs[i], soff, tt, hex);
                // try as gear struct right here and at 0xB0-strided array entries
                for (int goff = 0; goff < 0x100; goff += GEAR_SIZE) {
                    uint8_t fb2[GEAR_SIZE];
                    if (mem_read(target + goff, fb2, GEAR_SIZE) != 0) break;
                    uint16_t gid = fb2[0] | (fb2[1] << 8);
                    if (gid == 0) continue;
                    for (int t = 0; t < 3; t++) {
                        if (gid != g_loadout[t] || g_worn_info[t].id != 0) continue;
                        if (!gear_validate(fb2)) {
                            app_log("ptr[%d]+0x%X g%02X: id %u matches worn[%d] but invalid", i, soff, goff, gid, t);
                            continue;
                        }
                        gear_parse(&g_worn_info[t], fb2, t);
                        app_log("worn[%d] via ptr[%d]+0x%X+0x%02X: seed %08X rar %u main %u sub %u/%u/%u exp %u",
                                t, i, soff, goff, g_worn_info[t].seed, g_worn_info[t].rarity,
                                g_worn_info[t].primary, g_worn_info[t].subs[0],
                                g_worn_info[t].subs[1], g_worn_info[t].subs[2], g_worn_info[t].exp);
                    }
                }
                break;
            }
        }
        // Two-level chase: each record pointer targets an ARRAY of pointers
        // (seen in logs: ptr targets hold more 0x2E/0x69-style pointers).
        // Deref every entry, scan each for a gear struct at 4-byte offsets.
        static uint8_t l1[0x200];
        for (int i = 0; i < nptr && nmissing; i++) {
            if (mem_read(ptrs[i], l1, sizeof l1) != 0) continue;
            int chased = 0;
            for (int a = 0; a + 8 <= (int)sizeof l1; a += 8) {
                uint64_t p1 = 0;
                for (int k = 7; k >= 0; k--) p1 = (p1 << 8) | l1[a + k];
                if (p1 < 0x1000000000ULL || p1 >= 0x800000000000ULL) continue;
                uint8_t l2[0x40];
                if (mem_read(p1, l2, sizeof l2) != 0) continue;
                chased++;
                for (int o = 0; o + 4 <= (int)sizeof l2; o += 4) {
                    uint16_t gid = l2[o] | (l2[o + 1] << 8);
                    if (!gid) continue;
                    for (int t = 0; t < 3; t++) {
                        if (gid != g_loadout[t] || g_worn_info[t].id) continue;
                        uint8_t fb2[GEAR_SIZE];
                        if (mem_read(p1 + o, fb2, GEAR_SIZE) != 0) break;
                        if (!gear_validate(fb2)) {
                            app_log("chase ptr[%d]->%llX+0x%X: id %u matches worn[%d] but invalid", i,
                                    (unsigned long long)p1, o, gid, t);
                            continue;
                        }
                        gear_parse(&g_worn_info[t], fb2, t);
                        app_log("worn[%d] via chase ptr[%d]->%llX+0x%X: seed %08X rar %u main %u sub %u/%u/%u exp %u",
                                t, i, (unsigned long long)p1, o, g_worn_info[t].seed,
                                g_worn_info[t].rarity, g_worn_info[t].primary,
                                g_worn_info[t].subs[0], g_worn_info[t].subs[1],
                                g_worn_info[t].subs[2], g_worn_info[t].exp);
                        g_worn_addr[t] = p1 + o;
                    }
                }
            }
            app_log("chase ptr[%d] %llX: %d level-2 pointers dereferenced", i,
                    (unsigned long long)ptrs[i], chased);
        }

        for (int t = 0; t < 3; t++) if (g_loadout[t] && g_worn_info[t].id == 0) nmissing++;
        app_log("after chase, %d missing", nmissing);
        // fallback: content search if pointers didn't yield everything.
        // Skip entirely when all 3 are resolved (saves 3 full-heap walks).
        int need = 0;
        for (int t = 0; t < 3; t++) if (g_loadout[t] && g_worn_info[t].id == 0) need++;
        if (need == 0) {
            app_log("all 3 worn records resolved via pointer chase; skipping fallback scan");
        } else if (nmissing > 0) {
            app_log("fallback scan for %d missing record(s)", nmissing);
            // GEARLIST entries sit at a fixed 0x2C020 stride (upstream
            // GEARLIST_OFFS): worn records are list neighbors. Resolve the
            // most distinctive item first (clothes), then DERIVE the others
            // by stride — far faster and immune to scan junk.
            static const int order[3] = {1, 0, 2};
            for (int oi = 0; oi < 3; oi++) {
                int t = order[oi];
                if (!(g_loadout[t] && g_worn_info[t].id == 0)) continue;
                if (gear_find_struct(t, g_loadout[t], &g_worn_info[t])) {
                    g_worn_addr[t] = g_gear_found_addr;
                    app_log("worn[%d] via fallback content scan @%llX: seed %08X rar %u main %u sub %u/%u/%u",
                            t, (unsigned long long)g_gear_found_addr, g_worn_info[t].seed,
                            g_worn_info[t].rarity, g_worn_info[t].primary,
                            g_worn_info[t].subs[0], g_worn_info[t].subs[1], g_worn_info[t].subs[2]);
                }
                // stride-fill any still-missing slot from any resolved neighbor
                for (int t2 = 0; t2 < 3; t2++) {
                    if (!(g_loadout[t2] && g_worn_info[t2].id == 0)) continue;
                    for (int n = 0; n < 3; n++) {
                        if (!g_worn_addr[n] || n == t2) continue;
                        long long delta = ((long long)t2 - (long long)n) * 0x2C020LL;
                        uint64_t cand = g_worn_addr[n] + (uint64_t)delta;
                        uint8_t fb2[GEAR_SIZE];
                        if (mem_read(cand, fb2, GEAR_SIZE) != 0) continue;
                        uint16_t gid = fb2[0] | (fb2[1] << 8);
                        if (gid != g_loadout[t2] || !gear_validate(fb2)) continue;
                        gear_parse(&g_worn_info[t2], fb2, t2);
                        g_worn_addr[t2] = cand;
                        app_log("worn[%d] via stride %+lld from worn[%d] @%llX: seed %08X rar %u main %u sub %u/%u/%u",
                                t2, delta, n, (unsigned long long)cand, g_worn_info[t2].seed,
                                g_worn_info[t2].rarity, g_worn_info[t2].primary,
                                g_worn_info[t2].subs[0], g_worn_info[t2].subs[1], g_worn_info[t2].subs[2]);
                    }
                }
            }
        }
        const GearDataEntry* h = gear_data_find(0, g_loadout[0]);
        const GearDataEntry* c = gear_data_find(1, g_loadout[1]);
        const GearDataEntry* s = gear_data_find(2, g_loadout[2]);
        snprintf(g_status, sizeof(g_status), "Worn: %s / %s / %s",
                 h ? h->name : "?", c ? c->name : "?", s ? s->name : "?");
    }
    show_gear();
    if (g_attached) mem_detach();
}

// Debug: hunt for the raw u16 values of the anchor gear IDs in ALL readable
// regions, logging address + surrounding bytes for each hit. Tells us whether
// the game stores these IDs at all (and in what container).

// Minus: scan all three gear types, anchored on known owned-gear IDs first

// Y: dump a raw memory window around the FOUND gacha block for offline analysis.
// In the author's layout the gear lists sit ~15.5MB below the gacha block.
// 18MB total so it can be sent back over Telegram (20MB bot limit).

static void dump(void) {
    if (g_worn_info[0].id == 0 && g_worn_info[1].id == 0 && g_worn_info[2].id == 0) {
        snprintf(g_status, sizeof(g_status), "Read the worn items first (A).");
        return;
    }
    if (dump_json(NRO_PATH_OUT, g_worn_info, 3))
        snprintf(g_status, sizeof(g_status), "Exported worn gear -> leanny seed checker");
    else
        snprintf(g_status, sizeof(g_status), "Dump failed: %s", NRO_PATH_OUT);
    show_gear();
}

int main(void) {
    Result rc = romfsInit();
    if (R_FAILED(rc)) app_log("romfsInit failed: %x", rc);
    if (ui_init() != 0) {
        app_log("ui_init failed - no framebuffer");
        return 1;
    }
    show_gear();

    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    while (appletMainLoop()) {
        padUpdate(&pad);
        u64 k = padGetButtonsDown(&pad);
        if (k & HidNpadButton_Plus) {
            if (g_attached) mem_detach();
            break;
        }
        if (k & HidNpadButton_A) {
            read_loadout();
        }
        if (k & HidNpadButton_X) dump();
        show_gear();
        svcSleepThread(100000000ULL); // 100ms
    }
    ui_exit();
    romfsExit();
    return 0;
}
