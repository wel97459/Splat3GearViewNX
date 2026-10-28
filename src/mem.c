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
