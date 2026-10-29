#include "s3gv.h"
#include "generated/gear_tables.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define NRO_PATH_OUT "sdmc:/switch/Splat3GearViewNX/dump.json"
#define LOG_PATH "sdmc:/switch/Splat3GearViewNX/log.txt"
#define RAW_PATH "sdmc:/switch/Splat3GearViewNX/gearlist.bin"

static GearInfo g_gears[MAX_GEAR];
int g_gear_count = 0;
static int g_index = 0;
static int64_t g_delta = 0;
static int g_type_start[3] = {0, 0, 0};
static int g_type_count[3] = {0, 0, 0};
static uint64_t g_last_found = 0;   // last list address found by scanning
static uint64_t g_gacha_addr = 0;   // gacha block address found this read
uint16_t g_loadout[3] = {0, 0, 0}; // worn gear IDs (head/clothes/shoes), 0 = unknown
static int g_last_type = 0;
static char g_status[128] = "Press A to read gear from memory";
static bool g_attached = false;

static const char* TYPE_NAMES[3] = { "Headgear", "Clothes", "Shoes" };

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
static void hunt_ids(void) {
    static const uint16_t IDS[3] = { 1029, 8034, 3001 };
    app_log("hunt: starting raw-ID search");
    if (mem_attach() != 0) { snprintf(g_status, sizeof(g_status), "ERROR: could not attach."); return; }
    g_attached = true;
    static uint8_t buf[0x40000];
    for (int w = 0; w < 3; w++) {
        uint16_t want = IDS[w];
        int hits = 0, logged = 0;
        uint64_t addr = 0x8000000ULL;
        while (addr != 0) {
            uint64_t raddr, rsize; uint32_t rtype, rperm;
            if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) break;
            if (addr != raddr) addr = raddr;
            bool readable = (rperm & Perm_R) != 0;
            bool is_code = (rtype == MemType_CodeStatic || rtype == MemType_ModuleCodeStatic);
            if (readable && !is_code && rsize >= 0x100000) {
                uint64_t pos = 0;
                while (pos < rsize) {
                    size_t n = rsize - pos;
                    if (n > sizeof(buf)) n = sizeof(buf);
                    if (mem_read(raddr + pos, buf, n) == 0) {
                        for (size_t o = 0; o + 2 <= n; o++) {
                            if ((buf[o] | (buf[o+1] << 8)) != want) continue;
                            hits++;
                            if (logged < 48) {
                                logged++;
                                uint64_t ha = raddr + pos + o;
                                size_t s0 = o >= 8 ? o - 8 : 0;
                                size_t s1 = o + 24 <= n ? o + 24 : n;
                                char hex[(24 + 16) * 3 + 8] = "";
                                for (size_t k = s0; k < s1; k++)
                                    snprintf(hex + (k - s0) * 3, 4, "%02X ", buf[k]);
                                app_log("hit %u @%llX (u32=%s) ctx: %s", want,
                                        (unsigned long long)ha,
                                        (o + 4 <= n && (buf[o+2] | (buf[o+3] << 8)) == 0) ? "yes" : "no",
                                        hex);
                            }
                        }
                        pos += n;
                        if (n == sizeof(buf) && pos < rsize) pos -= 24; // keep context across chunk edge
                    } else {
                        pos += n;
                    }
                }
            }
            uint64_t next = raddr + rsize;
            if (next <= addr) break;
            addr = next;
        }
        app_log("hunt: ID %u total hits %d", want, hits);
    }
    snprintf(g_status, sizeof(g_status), "Hunt done, see log.txt");
    show_gear();
    if (g_attached) mem_detach();
}

// Minus: scan all three gear types, anchored on known owned-gear IDs first
static void rescan_next(void) {
    static const uint16_t ANCHOR_IDS[3] = { 1029, 8034, 3001 }; // Coral Cap / Fancyfish Stitch / Orange Arrows
    app_log("minus pressed: starting 3-type ID scan");
    snprintf(g_status, sizeof(g_status), "Scanning for owned gear (ID anchors)...");
    show_gear();
    if (mem_attach() != 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach.");
        return;
    }
    g_attached = true;
    static GearInfo out[MAX_GEAR];
    int total = 0;
    int counts[3] = {0, 0, 0};
    for (int t = 0; t < 3; t++) {
        snprintf(g_status, sizeof(g_status), "Scanning %s (anchor ID %u)...", TYPE_NAMES[t], ANCHOR_IDS[t]);
        show_gear();
        int run_len = 0;
        int64_t found = gear_locate_id(t, ANCHOR_IDS[t], &run_len);
        const char* how = "id-scan";
        if (found < 0) {
            found = gear_locate_inventory(t, &run_len, 0);
            how = "inventory-scan";
        }
        if (found < 0) { found = gear_locate_list(t, 0); how = "list-scan"; }
        if (found < 0) {
            app_log("%s: scan found nothing", TYPE_NAMES[t]);
            continue;
        }
        const char* status = NULL;
        int count = 0;
        if (!gear_read_list(out + total, MAX_GEAR - total, (uint64_t)found, 0, t, &status, &count) || count == 0) {
            app_log("%s: %s @%llX but read failed: %s", TYPE_NAMES[t], how,
                    (unsigned long long)found, status ? status : "?");
            continue;
        }
        app_log("%s: %s @%llX (run %d) -> %d gear", TYPE_NAMES[t], how,
                (unsigned long long)found, run_len, count);
        counts[t] = count;
        total += count;
        g_last_found = (uint64_t)found;
        g_last_type = t;
    }
    memcpy(g_gears, out, total * sizeof(GearInfo));
    g_gear_count = total;
    int s = 0;
    for (int t = 0; t < 3; t++) { g_type_start[t] = s; g_type_count[t] = counts[t]; s += counts[t]; }
    g_index = 0;
    snprintf(g_status, sizeof(g_status), "Scanned: %d gear (h%d c%d s%d)", total,
             counts[0], counts[1], counts[2]);
    show_gear();
    if (g_attached) mem_detach();
}

// Y: dump a raw memory window around the FOUND gacha block for offline analysis.
// In the author's layout the gear lists sit ~15.5MB below the gacha block.
// 18MB total so it can be sent back over Telegram (20MB bot limit).
static void dump_raw(void) {
    if (g_gacha_addr == 0) {
        snprintf(g_status, sizeof(g_status), "Press A first (need gacha anchor).");
        return;
    }
    if (mem_attach() != 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach.");
        return;
    }
    g_attached = true;
    uint64_t raddr = 0, rsize = 0; uint32_t rt = 0, rp = 0;
    mem_query(g_gacha_addr, &raddr, &rsize, &rt, &rp);
    app_log("dump region of gacha: %llX sz %llX", (unsigned long long)raddr, (unsigned long long)rsize);
    uint64_t want = g_gacha_addr - 0x1000000; // 16MB before
    uint64_t start = want > raddr ? want : raddr; // clamp to mapped region
    uint64_t total = g_gacha_addr + 0x200000 - start; // end 2MB after gacha
    FILE* f = fopen(RAW_PATH, "wb");
    if (!f) { snprintf(g_status, sizeof(g_status), "Open failed: %s", RAW_PATH); return; }
    static uint8_t buf[0x100000];
    uint64_t done = 0, good_end = start;
    while (done < total) {
        size_t chunk = total - done > sizeof(buf) ? sizeof(buf) : (size_t)(total - done);
        if (mem_read(start + done, buf, chunk) != 0) break; // stop at unmapped
        fwrite(buf, 1, chunk, f);
        done += chunk;
        good_end = start + done;
    }
    fclose(f);
    app_log("raw dump %llX..%llX -> %s", (unsigned long long)start, (unsigned long long)good_end, RAW_PATH);
    snprintf(g_status, sizeof(g_status), "Raw %uMB @%08llX (gacha at +16MB)",
             (unsigned)(done >> 20), (unsigned long long)start);
    show_gear();
    if (g_attached) mem_detach();
}

static void dump(void) {
    if (g_gear_count == 0) { snprintf(g_status, sizeof(g_status), "Nothing to dump."); return; }
    if (dump_json(NRO_PATH_OUT, g_gears, g_gear_count))
        snprintf(g_status, sizeof(g_status), "Dumped %d gear to %s", g_gear_count, NRO_PATH_OUT);
    else
        snprintf(g_status, sizeof(g_status), "Dump failed: %s", NRO_PATH_OUT);
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
        if (k & HidNpadButton_Y) dump_raw();
        if (k & HidNpadButton_ZR) hunt_ids();
        if (k & HidNpadButton_Minus) rescan_next();
        show_gear();
        svcSleepThread(100000000ULL); // 100ms
    }
    ui_exit();
    romfsExit();
    return 0;
}
