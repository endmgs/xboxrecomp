/*
 * xbox_fault_route.c - which deliberate trap a host fault is.
 *
 * The route recomp_fault_install is given (recomp_fault.h). It used to be
 * route_device_fault plus two watchpoint calls in every title's main.c, as
 * Windows-only code; here it is once, for every host, in the order main.c
 * had it:
 *
 *   1. A single-step: a watchpoint stepping over the write it let through
 *      (Windows, and any host with a trap flag).
 *   2. An access to a watched page (RECOMP_WATCH_WRITE). Before the device
 *      ranges, because a watch is a deliberate trap the devices know nothing
 *      about.
 *   3. A trapped device range: the vblank interrupt page, the APU's
 *      registers, the AC'97 page -- whatever registered itself with
 *      recomp_fault_add_range.
 *   4. POSIX only: the rest of a 16 KB host page that a 4 KB trap made
 *      inaccessible. The guest page allows the access, so it is completed
 *      through the arena's backdoor as if nothing had trapped.
 *
 * Anything else is a crash, and the title's report gets it.
 */
#include "xbox_fault_route.h"

#include "platform/fault_emulate.h"
#include "xbox_memory_layout.h"
#include "xbox_watchpoint.h"

extern ptrdiff_t g_xbox_mem_offset;

#include <stdio.h>
#include <stdlib.h>

/* RECOMP_FAULT_STATS=1: which guest pages the emulated faults land on.
 *
 * Every routed fault is a host signal and an emulated instruction, which costs
 * microseconds; a title whose working data shares a 16 KB host page with a
 * trapped 4 KB page, or whose code reads a device range in a loop, pays that
 * on every access and shows in a profile only as _sigtramp. This counts them
 * per 4 KB guest page, by route, and reports the busiest pages each time the
 * total doubles. Off by default; the counting is a few instructions. */
#define FAULT_STAT_SLOTS 64
static struct { uint32_t page; uint32_t n[2]; } g_fault_stat[FAULT_STAT_SLOTS];
static uint64_t g_fault_total, g_fault_next = 1024;

static void fault_stat(uint32_t va, int route)
{
    static int on = -1;
    uint32_t page = va & ~0xFFFu, h = (page >> 12) % FAULT_STAT_SLOTS;
    int i, k;

    if (on < 0) {
        const char *v = getenv("RECOMP_FAULT_STATS");
        on = v && *v && *v != '0';
    }
    if (!on)
        return;
    for (i = 0; i < FAULT_STAT_SLOTS; i++, h = (h + 1) % FAULT_STAT_SLOTS) {
        if (g_fault_stat[h].page == page || !g_fault_stat[h].n[0] && !g_fault_stat[h].n[1]) {
            g_fault_stat[h].page = page;
            g_fault_stat[h].n[route]++;
            break;
        }
    }
    if (++g_fault_total < g_fault_next)
        return;
    g_fault_next *= 2;
    fprintf(stderr, "[FAULT] %llu emulated faults; busiest guest pages (device route / 16 KB passthrough):\n",
            (unsigned long long)g_fault_total);
    {
        unsigned char shown[FAULT_STAT_SLOTS] = { 0 };
        for (k = 0; k < 8; k++) {
            int best = -1;
            uint32_t bn = 0;
            for (i = 0; i < FAULT_STAT_SLOTS; i++) {
                uint32_t n = g_fault_stat[i].n[0] + g_fault_stat[i].n[1];
                if (!shown[i] && n > bn) { bn = n; best = i; }
            }
            if (best < 0)
                break;
            shown[best] = 1;
            fprintf(stderr, "[FAULT]   0x%08X  %u / %u", g_fault_stat[best].page,
                    g_fault_stat[best].n[0], g_fault_stat[best].n[1]);
#ifndef _WIN32
            {   /* the guest protection of the 4 KB pages sharing its 16 KB host page */
                uint32_t hp = g_fault_stat[best].page & ~0x3FFFu, q;
                fprintf(stderr, "   host page 0x%08X:", hp);
                for (q = 0; q < 4; q++)
                    fprintf(stderr, " %lX", (unsigned long)w32_page_protection(
                        (const void *)((uintptr_t)(hp + q * 0x1000u) + (uintptr_t)g_xbox_mem_offset)));
            }
#endif
            fprintf(stderr, "\n");
        }
    }
}

int xbox_fault_route(recomp_fault *f)
{
    uintptr_t base = (uintptr_t)g_xbox_mem_offset;
    uint32_t va;

    if (f->kind == RECOMP_FAULT_SINGLE_STEP)
        return xbox_watch_handle_trap(f);
    if (f->kind != RECOMP_FAULT_ACCESS)
        return 0;

    if (xbox_watch_handle_fault(f))
        return 1;

    if (!base || f->host_addr < base || f->host_addr - base > 0xFFFFFFFFu)
        return 0;
    va = (uint32_t)(f->host_addr - base);
    if (recomp_fault_dispatch_ranges(f, va)) {
        fault_stat(va, 0);
        return 1;
    }

    if (recomp_fault_passthrough(f)) {     /* 0 on Windows */
        fault_stat(va, 1);
        return 1;
    }
    return 0;
}
