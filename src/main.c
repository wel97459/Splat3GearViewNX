#include "s3gv.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>

#define NRO_PATH_OUT "sd:/switch/Splat3GearViewNX/dump.json"

static GearInfo g_gears[MAX_GEAR];
static int g_gear_count = 0;
static int g_index = 0;
static char g_status[128] = "Press A to read gear from memory";
static bool g_attached = false;

static const char* TYPE_NAMES[3] = { "Headgear", "Clothes", "Shoes" };

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
    snprintf(g_status, sizeof(g_status), "Attaching to game...");
    show_gear();

    if (mem_attach() != 0) {
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

    // Gear lists
    uint64_t bases[3] = { GEARLIST_HEAD + delta, GEARLIST_CLOTHES + delta, GEARLIST_SHOES + delta };
    for (int t = 0; t < 3; t++) {
        snprintf(g_status, sizeof(g_status), "Reading %s...", TYPE_NAMES[t]);
        show_gear();
        const char* status = NULL;
        int count = 0;
        int space = MAX_GEAR - g_gear_count;
        if (!gear_read_list(g_gears + g_gear_count, space, bases[t] + 0x40, t, &status, &count)) {
            snprintf(g_status, sizeof(g_status), "ERROR reading %s: %s", TYPE_NAMES[t], status ? status : "?");
            return;
        }
        g_gear_count += count;
    }
    snprintf(g_status, sizeof(g_status), "Done. %d gear%s", g_gear_count, seedinfo);
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
