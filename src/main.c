#include "s3gv.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define NRO_PATH_OUT "sdmc:/switch/Splat3GearViewNX/dump.json"
#define LOG_PATH "sdmc:/switch/Splat3GearViewNX/log.txt"
#define RAW_PATH "sdmc:/switch/Splat3GearViewNX/gearlist.bin"

static GearInfo g_gears[MAX_GEAR];
static int g_gear_count = 0;
static int g_index = 0;
static int64_t g_delta = 0;
static int g_type_start[3] = {0, 0, 0};
static int g_type_count[3] = {0, 0, 0};
static uint64_t g_last_found = 0;   // last list address found by scanning
static int g_last_type = 0;
static char g_status[128] = "Press A to read gear from memory";
static bool g_attached = false;

static const char* TYPE_NAMES[3] = { "Headgear", "Clothes", "Shoes" };

static void app_log(const char* fmt, ...) {
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
    consoleClear();
    printf("\x1b[1;32mSplat3GearView NX\x1b[0m  -  %d gear loaded\n\n", g_gear_count);
    printf("\x1b[2m%s\x1b[0m\n\n", g_status);

    if (g_gear_count == 0) {
        printf("No gear loaded yet.\n\n");
    } else {
        GearInfo* g = &g_gears[g_index];
        printf("Gear %d / %d  [%s]\n\n", g_index + 1, g_gear_count, TYPE_NAMES[g->gear_type]);
        printf("Name:   %s\n", g->name);
        printf("Brand:  %s\n", g->brand);
        printf("ID:     %05u\n", g->id);
        printf("Seed:   %08X\n", g->seed);
        printf("Rarity: %u   Slots: %u\n\n", g->rarity, g->sub_count);
        printf("Primary:  %s\n", ability_name(g->primary));
        printf("Sub 1:    %s\n", ability_name(g->subs[0]));
        if (g->rarity >= 1) printf("Sub 2:    %s\n", ability_name(g->subs[1]));
        if (g->rarity >= 2) printf("Sub 3:    %s\n", ability_name(g->subs[2]));
    }

    printf("\n\x1b[2mControls:\n"
           "  A: read   Up: raw fixed   -: rescan from last match\n"
           "  Y: raw mem dump   L/R: prev/next   B(+): exit\x1b[0m\n");
    consoleUpdate(NULL);
}

static void read_all(int use_delta) {
    g_gear_count = 0;
    g_index = 0;
    app_log("---- read start ----");
    snprintf(g_status, sizeof(g_status), "Attaching to game...");
    show_gear();

    if (mem_attach() != 0) {
        app_log("ERROR: attach failed");
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach. Game must be running (suspended) behind hbmenu.");
        return;
    }
    g_attached = true;

    // Environment diagnostics (sys-botbase style): heap base, NSO base, game version
    uint64_t heap_base = mem_heap_base();
    uint64_t nso_base = mem_main_nso_base();
    u64 title_id = 0;
    u32 game_version = 0;
    NsApplicationContentMetaStatus meta[4];
    s32 nmeta = 0;
    if (R_FAILED(nsListApplicationContentMetaStatus(0, 0, meta, 4, &nmeta))) nmeta = 0;
    for (s32 i = 0; i < nmeta; i++) {
        if (meta[i].version > game_version) game_version = meta[i].version;
        title_id = meta[i].application_id;
    }
    game_version /= 0x10000;
    app_log("env: heap_base=%llX nso_base=%llX title=%016llX ver=%u.%u.%u",
            (unsigned long long)heap_base, (unsigned long long)nso_base,
            (unsigned long long)title_id,
            game_version / 100, (game_version / 10) % 10, game_version % 10);
    app_log("env: fixed GACHA_BLOCK=%llX GEARLIST_HEAD=%llX (author console addresses)",
            (unsigned long long)GACHA_BLOCK, (unsigned long long)GEARLIST_HEAD);

    // Resolve heap shift: validate the fixed gacha address by content, not just
    // by whether the read succeeds (heap regions are GBs; a wrong address still maps).
    // Up on the D-pad forces the pure author addresses with no delta at all.
    int64_t delta = 0;
    GachaSeed seeds[8];
    int nseeds = 0;
    if (use_delta) {
        nseeds = gacha_read(GACHA_BLOCK, seeds, 8);
        if (nseeds <= 0) nseeds = gacha_read(GACHA_BLOCK + DLC_OFFS, seeds, 8);
        if (nseeds <= 0) {
            snprintf(g_status, sizeof(g_status), "Fixed address wrong, scanning heap...");
            show_gear();
            int64_t found = mem_locate_gacha();
            if (found < 0) {
                uint64_t raddr = 0, rsize = 0; uint32_t rtype = 0, rperm = 0;
                mem_query(GACHA_BLOCK, &raddr, &rsize, &rtype, &rperm);
                snprintf(g_status, sizeof(g_status),
                         "ERROR: gacha keys not found. map@%08llX region=%08llX sz=%08llX t=%u p=%u",
                         (unsigned long long)GACHA_BLOCK, (unsigned long long)raddr,
                         (unsigned long long)rsize, rtype, rperm);
                show_gear();
                return;
            }
            delta = found - (int64_t)GACHA_BLOCK;
            snprintf(g_status, sizeof(g_status), "Heap shifted by %llX, reading...", (long long)delta);
            show_gear();
            nseeds = gacha_read(GACHA_BLOCK + delta, seeds, 8);
        }
    } else {
        app_log("raw mode: using pure author addresses, no delta");
        nseeds = gacha_read(GACHA_BLOCK, seeds, 8);
    }

    char seedinfo[64] = "";
    if (nseeds > 0)
        snprintf(seedinfo, sizeof(seedinfo), " (seeds: %d, %08X)", nseeds, seeds[0].s0);

    // Gear lists: try the fixed offset first; if it yields nothing, scan the
    // heap for a run of valid structs of this type (start after the gacha block)
    uint64_t bases[3] = { GEARLIST_HEAD + delta, GEARLIST_CLOTHES + delta, GEARLIST_SHOES + delta };
    for (int t = 0; t < 3; t++) {
        snprintf(g_status, sizeof(g_status), "Reading %s...", TYPE_NAMES[t]);
        show_gear();
        const char* status = NULL;
        int count = 0;
        int space = MAX_GEAR - g_gear_count;
        uint64_t base = bases[t];
        uint64_t start = 0x40;
        if (!gear_read_list(g_gears + g_gear_count, space, base, start, t, &status, &count) || count == 0) {
            // Show what's actually at the fixed address; user decides whether to scan (Minus)
            uint8_t dbg[0x20];
            char hex[0x20 * 3 + 8] = "";
            if (mem_read(base + start, dbg, sizeof(dbg)) == 0) {
                for (size_t i = 0; i < sizeof(dbg); i++)
                    snprintf(hex + i * 3, 4, "%02X ", dbg[i]);
            } else {
                snprintf(hex, sizeof(hex), "unreadable");
            }
            app_log("%s: fixed @%llX+%llX gave 0 (status=%s) data: %s",
                    TYPE_NAMES[t], (unsigned long long)base, (unsigned long long)start,
                    status ? status : "0-found", hex);
            snprintf(g_status, sizeof(g_status), "%s fixed: %s", TYPE_NAMES[t], hex);
            g_delta = delta;
            show_gear();
            return;
        }
        g_gear_count += count;
        app_log("%s: %d gear", TYPE_NAMES[t], count);
        g_type_start[t] = g_gear_count - count;
        g_type_count[t] = count;
        if (g_last_found == 0) g_last_found = base;
        g_last_type = t;
    }
    snprintf(g_status, sizeof(g_status), "Done. %d gear (delta %llX)%s",
             g_gear_count, (long long)delta, seedinfo);
}

// Minus: continue scanning for the current type's list after the last found address
static void rescan_next(void) {
    if (g_last_found == 0) { snprintf(g_status, sizeof(g_status), "Nothing scanned yet."); return; }
    int t = g_last_type;
    snprintf(g_status, sizeof(g_status), "Scanning %s after last match...", TYPE_NAMES[t]);
    show_gear();
    if (mem_attach() != 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: could not attach.");
        return;
    }
    g_attached = true;
    uint64_t start = g_last_found + GEAR_SIZE;
    int inv_len = 0;
    int64_t found = gear_locate_inventory(t, &inv_len, start);
    const char* how = "inventory-scan";
    if (found < 0) { found = gear_locate_list(t, start); how = "list-scan"; }
    if (found < 0) {
        snprintf(g_status, sizeof(g_status), "%s: no further match found", TYPE_NAMES[t]);
        app_log("%s: rescan after %llX found nothing", TYPE_NAMES[t], (unsigned long long)start);
        show_gear();
        if (g_attached) mem_detach();
        return;
    }
    g_last_found = (uint64_t)found;
    app_log("%s: rescan %s @%llX (len %d)", TYPE_NAMES[t], how,
            (unsigned long long)found, inv_len);
    // Read the new list into a temp buffer
    static GearInfo tmp[128];
    const char* status = NULL;
    int count = 0;
    if (!gear_read_list(tmp, 128, (uint64_t)found, 0, t, &status, &count) || count == 0) {
        snprintf(g_status, sizeof(g_status), "ERROR: rescan read failed: %s", status ? status : "?");
        show_gear();
        if (g_attached) mem_detach();
        return;
    }
    // Replace this type's segment in the main array
    int s = g_type_start[t], oldc = g_type_count[t];
    int tail = g_gear_count - (s + oldc);
    int newc = count;
    int shift = newc - oldc;
    if (shift != 0 && tail > 0)
        memmove(g_gears + s + newc, g_gears + s + oldc, tail * sizeof(GearInfo));
    memcpy(g_gears + s, tmp, newc * sizeof(GearInfo));
    g_type_count[t] = newc;
    g_gear_count += shift;
    // Fix up start offsets of later types
    for (int u = t + 1; u < 3; u++) g_type_start[u] += shift;
    if (g_index >= g_gear_count) g_index = g_gear_count ? g_gear_count - 1 : 0;
    snprintf(g_status, sizeof(g_status), "%s: %d gear @%llX (%s)", TYPE_NAMES[t], newc,
             (unsigned long long)found, how);
    show_gear();
    if (g_attached) mem_detach();
}

// Y: dump a raw memory window around the (shifted) gear list for offline analysis
static void dump_raw(void) {
    if (!g_attached) {
        snprintf(g_status, sizeof(g_status), "Attach first (X).");
        return;
    }
    uint64_t base = GEARLIST_HEAD + g_delta;
    #define RAW_BEFORE 0x8000
    const uint64_t BEFORE = RAW_BEFORE;
#define RAW_LEN 0x40000
    const uint64_t LEN = RAW_LEN;
    static uint8_t buf[RAW_LEN];
    if (mem_read(base - BEFORE, buf, LEN) != 0) {
        snprintf(g_status, sizeof(g_status), "Raw read failed @%08llX", (unsigned long long)(base - BEFORE));
        return;
    }
    FILE* f = fopen(RAW_PATH, "wb");
    if (!f) { snprintf(g_status, sizeof(g_status), "Open failed: %s", RAW_PATH); return; }
    fwrite(buf, 1, LEN, f);
    fclose(f);
    app_log("raw dump %llX..%llX -> %s", (unsigned long long)(base - BEFORE),
         (unsigned long long)(base - BEFORE + LEN), RAW_PATH);
    snprintf(g_status, sizeof(g_status), "Raw dump -> %s (list at +0x%X)",
             RAW_PATH, (unsigned)(BEFORE + 0x40));
}

static void dump(void) {
    if (g_gear_count == 0) { snprintf(g_status, sizeof(g_status), "Nothing to dump."); return; }
    if (dump_json(NRO_PATH_OUT, g_gears, g_gear_count))
        snprintf(g_status, sizeof(g_status), "Dumped %d gear to %s", g_gear_count, NRO_PATH_OUT);
    else
        snprintf(g_status, sizeof(g_status), "Dump failed: %s", NRO_PATH_OUT);
}

int main(void) {
    consoleInit(NULL);
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
            read_all(1);
            if (g_attached) mem_detach();
        }
        if (k & HidNpadButton_X) dump();
        if (k & HidNpadButton_Y) dump_raw();
        if (k & HidNpadButton_Minus) rescan_next();
        if (k & HidNpadButton_Up) read_all(0); // raw mode: pure author addresses
        if (g_gear_count > 0) {
            if (k & HidNpadButton_R) g_index = (g_index + 1) % g_gear_count;
            if (k & HidNpadButton_L) g_index = (g_index + g_gear_count - 1) % g_gear_count;
        }
        show_gear();
        svcSleepThread(100000000ULL); // 100ms
    }
    consoleExit(NULL);
    return 0;
}
