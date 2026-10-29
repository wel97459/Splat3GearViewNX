#include "s3gv.h"
#include <switch.h>
#include <stdio.h>
#include <string.h>

static Handle g_debug = INVALID_HANDLE;
static bool g_attached = false;

static Result get_app_pid(u64* pid) {
    Result rc = pmdmntInitialize();
    if (R_FAILED(rc)) return rc;
    rc = pmdmntGetApplicationProcessId(pid);
    pmdmntExit();
    return rc;
}

int mem_attach(void) {
    if (g_attached) return 0;
    u64 pid = 0;
    Result rc = get_app_pid(&pid);
    if (R_FAILED(rc)) return -1;
    rc = svcDebugActiveProcess(&g_debug, pid);
    if (R_FAILED(rc)) return -1;
    g_attached = true;
    return 0;
}

void mem_detach(void) {
    if (!g_attached) return;
    svcCloseHandle(g_debug);
    g_debug = INVALID_HANDLE;
    g_attached = false;
}

int mem_read(uint64_t addr, void* buf, size_t size) {
    if (!g_attached) return -1;
    Result rc = svcReadDebugProcessMemory(buf, g_debug, addr, size);
    return R_FAILED(rc) ? -1 : 0;
}

// Query the mapped region containing (or following) addr. 0 on success.
int mem_query(uint64_t addr, uint64_t* raddr, uint64_t* rsize, uint32_t* rtype, uint32_t* rperm) {
    if (!g_attached) return -1;
    MemoryInfo info;
    u32 pageinfo;
    Result rc = svcQueryDebugProcessMemory(&info, &pageinfo, g_debug, addr);
    if (R_FAILED(rc)) return -1;
    if (raddr) *raddr = info.addr;
    if (rsize) *rsize = info.size;
    if (rtype) *rtype = info.type;
    if (rperm) *rperm = info.perm;
    return 0;
}

#define SCAN_CHUNK 0x10000

// Check a buffer for any of the 5 gacha keys; verify candidate with a window read.
static bool scan_for_gacha(const uint8_t* buf, size_t n, uint64_t base, int64_t* found) {
    static const uint32_t keys[5] = {
        GACHA_KEY_NORMAL, GACHA_KEY_FEST, GACHA_KEY_MURCH, GACHA_KEY_TITLE, GACHA_KEY_BANNER
    };
    if (n < 4) return false;
    for (size_t i = 0; i + 4 <= n; i++) {
        uint32_t v = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8) |
                     ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
        bool iskey = false;
        for (int k = 0; k < 5; k++)
            if (v == keys[k]) { iskey = true; break; }
        if (!iskey) continue;
        // Verify: read a 0x900 window starting here and require >= 2 keys total
        uint8_t win[GACHA_SIZE];
        if (mem_read(base + i, win, sizeof(win)) != 0) continue;
        int hits = 0;
        for (uint32_t o = 0; o + 4 <= GACHA_SIZE; o += 4) {
            uint32_t w = (uint32_t)win[o] | ((uint32_t)win[o+1] << 8) |
                         ((uint32_t)win[o+2] << 16) | ((uint32_t)win[o+3] << 24);
            for (int k = 0; k < 5; k++)
                if (w == keys[k]) { hits++; break; }
        }
        if (hits >= 2) {
            *found = (int64_t)(base + i);
            return true;
        }
    }
    return false;
}

// Walk the game's memory map and scan readable regions for the gacha key block.
// Returns the found block address, or -1.
int64_t mem_locate_gacha(void) {
    if (!g_attached) return -1;
    uint64_t addr = 0x8000000ULL; // skip the low unmapped hole
    uint8_t buf[SCAN_CHUNK];
    while (addr != 0) {
        uint64_t raddr, rsize;
        uint32_t rtype, rperm;
        if (mem_query(addr, &raddr, &rsize, &rtype, &rperm) != 0) return -1;
        if (addr != raddr) addr = raddr; // query rounds down to region start
        bool readable = (rperm & Perm_R) != 0;
        bool is_code = (rtype == MemType_CodeStatic || rtype == MemType_ModuleCodeStatic);
        if (readable && !is_code && rsize >= 0x100000) {
            uint64_t pos = 0;
            while (pos < rsize) {
                size_t n = rsize - pos;
                if (n > SCAN_CHUNK) n = SCAN_CHUNK;
                if (mem_read(raddr + pos, buf, n) == 0) {
                    int64_t found = -1;
                    if (scan_for_gacha(buf, n, raddr + pos, &found)) return found;
                }
                pos += n;
                if (n == SCAN_CHUNK && pos < rsize) pos -= 4; // overlap for boundary keys
            }
        }
        uint64_t next = raddr + rsize;
        if (next <= addr) break; // wrapped or done
        addr = next;
    }
    return -1;
}
