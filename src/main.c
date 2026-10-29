#include "s3gv.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define NRO_PATH_OUT "sd:/switch/Splat3GearViewNX/dump.json"
#define LOG_PATH "sd:/switch/Splat3GearViewNX/log.txt"
#define RAW_PATH "sd:/switch/Splat3GearViewNX/gearlist.bin"

static GearInfo g_gears[MAX_GEAR];
static int g_gear_count = 0;
static int g_index = 0;
static int64_t g_delta = 0;
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
           "  A: read gear   L/R: prev/next\n"
           "  X: dump JSON to %s\n"
           "  B: detach & exit\x1b[0m\n", NRO_PATH_OUT);
    consoleUpdate(NULL);
}

static void read_all(void) {
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

    // Resolve heap shift: validate the fixed gacha address by content, not just
    // by whether the read succeeds (heap regions are GBs; a wrong address still maps).
    int64_t delta = 0;
    GachaSeed seeds[8];
    int nseeds = gacha_read(GACHA_BLOCK, seeds, 8);
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
            app_log("%s: fixed offset empty (status=%s), scanning heap...", TYPE_NAMES[t], status ? status : "0-found");
            snprintf(g_status, sizeof(g_status), "%s: fixed offset empty, scanning heap...", TYPE_NAMES[t]);
            show_gear();
            int64_t found = gear_locate_list(t);
            if (found < 0) {
                snprintf(g_status, sizeof(g_status), "ERROR: %s list not found by scan", TYPE_NAMES[t]);
                app_log("%s: scan failed", TYPE_NAMES[t]);
                show_gear();
                return;
            }
            base = (uint64_t)found;
            start = 0;
            app_log("%s: scan found list @%llX (expected %llX, rel %+llX)",
                    TYPE_NAMES[t], (unsigned long long)found,
                    (unsigned long long)bases[t], (long long)(found - (int64_t)bases[t]));
            count = 0;
            if (!gear_read_list(g_gears + g_gear_count, space, base, start, t, &status, &count) || count == 0) {
                snprintf(g_status, sizeof(g_status), "ERROR: %s read failed after scan: %s",
                         TYPE_NAMES[t], status ? status : "?");
                show_gear();
                return;
            }
        }
        g_gear_count += count;
        app_log("%s: %d gear", TYPE_NAMES[t], count);
    }
    snprintf(g_status, sizeof(g_status), "Done. %d gear (delta %llX)%s",
             g_gear_count, (long long)delta, seedinfo);
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
            read_all();
            if (g_attached) mem_detach();
        }
        if (k & HidNpadButton_X) dump();
        if (k & HidNpadButton_Y) dump_raw();
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
