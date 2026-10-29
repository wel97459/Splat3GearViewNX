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
static uint64_t g_gacha_addr = 0;   // gacha block address found this read
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
           "  A: read   ZR: hunt IDs   -: ID scan\n"
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
    u64 pid = 0;
    Result pmrc = pminfoInitialize();
    if (R_SUCCEEDED(pmrc)) {
        u64 pid2 = 0;
        if (R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid2)) && R_SUCCEEDED(pminfoGetProgramId(&title_id, pid2)))
            pid = pid2;
        pminfoExit();
    }
    NsApplicationContentMetaStatus meta[4];
    s32 nmeta = 0;
    if (R_FAILED(nsListApplicationContentMetaStatus(title_id, 0, meta, 4, &nmeta))) nmeta = 0;
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

    g_gacha_addr = GACHA_BLOCK + delta;
    app_log("gacha anchor @%llX (delta %+llX, seeds %d)", (unsigned long long)g_gacha_addr, (long long)delta, nseeds);
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
        if (k & HidNpadButton_ZR) hunt_ids();
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
