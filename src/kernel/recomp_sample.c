/**
 * A sampling profiler for the whole process, by thread and by native symbol.
 *
 * The entry profiler (RECOMP_TRACE_PROFILE) counts calls, and a count says
 * where the calls go, not where the time goes: a function entered once that
 * loops for a frame is invisible to it, and a leaf entered a million times
 * cheaply dominates it. The question a slow title asks -- is this one hot path
 * or a diffuse cost spread over thousands of lifted functions -- needs time,
 * so this samples the instruction pointer instead.
 *
 * Every RECOMP_SAMPLE-th of a second, each thread in the process is suspended
 * long enough to read its context and walk a few frames of its native stack,
 * and the addresses are tallied. Nothing is resolved while a thread is
 * suspended: dbghelp takes locks the suspended thread may hold, so symbols are
 * looked up only at report time, from the sampler thread, with everything
 * running. Every generated function is a public symbol in the image (the title
 * links with /DEBUG), so a lifted address resolves to its sub_XXXXXXXX.
 *
 *   RECOMP_SAMPLE=<hz>          sample at this rate (1000 is fine)
 *   RECOMP_SAMPLE_REPORT=<s>    print a summary to stderr every s seconds (10)
 *   RECOMP_SAMPLE_DUMP=<path>   write the full per-symbol table there, rewritten
 *                               at every report so a killed run leaves one
 *   RECOMP_SAMPLE_DEPTH=<n>     native frames per sample (8; 1 is leaf only)
 *   RECOMP_SAMPLE_START=<s>     take no samples for the first s seconds, so
 *                               the report is the state a run settles into
 *                               (a hang an input script reaches after two
 *                               minutes) rather than everything before it
 *
 * Two views come out. "Leaf" is the function the thread was executing:
 * exclusive time, which names the hot instruction sequences. "Inclusive" is
 * every function on the sampled stack, counted once per sample, which says
 * what a leaf's cost belongs to -- a memcpy is not interesting, the lifted
 * function that called it is. Categories are by symbol prefix and module:
 * sub_ is lifted game code, hle_ the HLE layer, d3d8_ the host renderer's
 * translation layer, bridge_/xbox_/kernel_ the kernel bridge, recomp_ the
 * runtime (dispatch and the trace hook), and DLLs by name.
 *
 * ponytail: the native stack walk needs unwind data, which every frame in the
 * image and in system DLLs has; it stops at the first frame it cannot unwind.
 * A sample taken while a thread is in the kernel (a wait) shows the syscall
 * stub in ntdll, which is what "waiting" means here. The sampler itself costs
 * about 10 us per thread per sample, so 1 kHz over a dozen threads is roughly
 * a tenth of a core -- run it at 250 for a measurement, 1000 for a profile.
 */

/*
 * macOS: the same sampler on Mach. task_threads() lists the threads,
 * thread_suspend() and thread_get_state() read each one's pc and frame
 * pointer, the walk follows the frame-pointer chain (which every function on
 * Apple's platforms keeps) through mach_vm_read_overwrite so a bad pointer
 * ends the walk instead of faulting, and dladdr() names addresses at report
 * time. Lifted functions resolve to their sub_XXXXXXXX as long as the
 * executable is not stripped.
 */

#if defined(_WIN32) || defined(__APPLE__)
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <dbghelp.h>
#include <psapi.h>
#else
#include "win32_compat.h"
#include <dlfcn.h>
#include <pthread.h>
#include <time.h>
#include <wchar.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach-o/dyld.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbox_memory_layout.h"

#define SAMPLE_MAX_THREADS  48
#define SAMPLE_SLOTS        (1u << 16)          /* per table, power of two */
#define SAMPLE_MAX_DEPTH    32

typedef struct {
    uintptr_t addr;
    unsigned  count;
} SampleSlot;

/* The leaf and the three frames above it, as one key. "getenv is 7% of the
 * thread" names a cost; this names who pays it, which is what decides the
 * fix. Kept per thread beside the leaf and inclusive tables. */
#define SAMPLE_CHAIN        6
#define SAMPLE_CHAIN_SLOTS  (1u << 13)
typedef struct {
    uintptr_t f[SAMPLE_CHAIN];
    unsigned  count;
} SampleChain;

typedef struct {
    DWORD      tid;
#ifdef _WIN32
    HANDLE     handle;
#else
    thread_act_t handle;     /* a send right this table owns */
#endif
    int        alive;
    char       name[48];
    unsigned   samples;
    SampleSlot *leaf;        /* exclusive: the instruction pointer */
    SampleSlot *incl;        /* inclusive: every frame on the stack, once */
    SampleChain *chain;      /* leaf plus its callers, for the report's "via" lines */
    unsigned   leaf_used, incl_used, chain_used;
    ULONGLONG  cpu_100ns_at_start;
} SampleThread;

static SampleThread s_threads[SAMPLE_MAX_THREADS];
static int   s_thread_count;
static DWORD s_self_tid, s_main_tid;
static int   s_depth = 8;
static double s_hz = 1000.0;
static double s_report_secs = 10.0;
static LARGE_INTEGER s_qpf, s_t0;
static unsigned long long s_total_samples;
static HMODULE s_exe;
static uintptr_t s_exe_lo, s_exe_hi;
static CRITICAL_SECTION s_lock;      /* the tables, between sampling and reporting */

/* ---------------------------------------------------------------- tallying */

static void slot_add(SampleSlot *table, unsigned *used, uintptr_t addr)
{
    unsigned i = (unsigned)((addr >> 2) * 2654435761u) & (SAMPLE_SLOTS - 1);
    unsigned n;

    for (n = 0; n < SAMPLE_SLOTS; n++, i = (i + 1) & (SAMPLE_SLOTS - 1)) {
        if (table[i].addr == addr) { table[i].count++; return; }
        if (!table[i].addr) {
            table[i].addr = addr;
            table[i].count = 1;
            (*used)++;
            return;
        }
    }
}

/* The leaf and up to three callers, keyed together. A full table drops new
 * chains rather than evicting: the hot ones were there first. */
static void chain_add(SampleChain *table, unsigned *used, const uintptr_t *fr, int n)
{
    uintptr_t f[SAMPLE_CHAIN];
    unsigned h = 0, i, k;

    for (k = 0; k < SAMPLE_CHAIN; k++) {
        f[k] = (int)k < n ? fr[k] : 0;
        h = h * 2654435761u + (unsigned)(f[k] >> 2);
    }
    i = h & (SAMPLE_CHAIN_SLOTS - 1);
    for (k = 0; k < SAMPLE_CHAIN_SLOTS; k++, i = (i + 1) & (SAMPLE_CHAIN_SLOTS - 1)) {
        if (table[i].count && memcmp(table[i].f, f, sizeof f) == 0) {
            table[i].count++;
            return;
        }
        if (!table[i].count) {
            memcpy(table[i].f, f, sizeof f);
            table[i].count = 1;
            (*used)++;
            return;
        }
    }
}

#ifdef _WIN32
typedef void (WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);
typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);

static void thread_name(SampleThread *t)
{
    static GetThreadDescription_t get_desc;
    static int looked;
    PWSTR desc = NULL;

    if (!looked) {
        looked = 1;
        get_desc = (GetThreadDescription_t)(void *)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    }
    if (t->tid == s_main_tid) {
        strcpy(t->name, "guest main");
        return;
    }
    if (get_desc && SUCCEEDED(get_desc(t->handle, &desc)) && desc) {
        if (desc[0])
            snprintf(t->name, sizeof t->name, "%ls", desc);
        LocalFree(desc);
    }
    if (!t->name[0])
        snprintf(t->name, sizeof t->name, "thread %lu", (unsigned long)t->tid);
}

/* Keep the handle table current: threads come and go. Once a second. */
static void refresh_threads(void)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    int i;

    if (snap == INVALID_HANDLE_VALUE)
        return;
    for (i = 0; i < s_thread_count; i++)
        s_threads[i].alive = 0;

    te.dwSize = sizeof te;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == s_self_tid)
                continue;
            for (i = 0; i < s_thread_count; i++)
                if (s_threads[i].tid == te.th32ThreadID)
                    break;
            if (i < s_thread_count) {
                s_threads[i].alive = 1;
                continue;
            }
            if (s_thread_count >= SAMPLE_MAX_THREADS)
                continue;
            {
                SampleThread *t = &s_threads[s_thread_count];
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                      | THREAD_QUERY_INFORMATION, FALSE,
                                      te.th32ThreadID);
                if (!h)
                    continue;
                memset(t, 0, sizeof *t);
                t->tid = te.th32ThreadID;
                t->handle = h;
                t->alive = 1;
                t->leaf = (SampleSlot *)calloc(SAMPLE_SLOTS, sizeof(SampleSlot));
                t->incl = (SampleSlot *)calloc(SAMPLE_SLOTS, sizeof(SampleSlot));
                t->chain = (SampleChain *)calloc(SAMPLE_CHAIN_SLOTS, sizeof(SampleChain));
                if (!t->leaf || !t->incl || !t->chain) {
                    free(t->leaf); free(t->incl); free(t->chain); CloseHandle(h);
                    continue;
                }
                {
                    FILETIME c, e, k, u;
                    if (GetThreadTimes(h, &c, &e, &k, &u))
                        t->cpu_100ns_at_start =
                            (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime)
                          + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime);
                }
                thread_name(t);
                s_thread_count++;
            }
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
}

/* Read one thread's instruction pointer and a few return addresses. The thread
 * is suspended for the duration, so its stack is stable; the unwind itself
 * reads the stack and the image's unwind tables and nothing else. A context
 * that cannot be unwound simply ends the walk. */
static int sample_thread(SampleThread *t, uintptr_t *frames, int max_frames)
{
    CONTEXT ctx;
    int n = 0;

    if (SuspendThread(t->handle) == (DWORD)-1)
        return 0;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(t->handle, &ctx)) {
        frames[n++] = (uintptr_t)ctx.Rip;
        __try {
            while (n < max_frames && ctx.Rip) {
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, NULL);
                if (!fn) {
                    /* A leaf function with no unwind data: the return address
                     * is at [rsp]. */
                    ctx.Rip = *(DWORD64 *)(uintptr_t)ctx.Rsp;
                    ctx.Rsp += 8;
                } else {
                    PVOID handler_data = NULL;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn,
                                     &ctx, &handler_data, &establisher, NULL);
                }
                if (!ctx.Rip)
                    break;
                frames[n++] = (uintptr_t)ctx.Rip;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* the walk ran off the stack; keep what was read */
        }
    }
    ResumeThread(t->handle);
    return n;
}

static double thread_cpu_seconds(const SampleThread *t)
{
    FILETIME ct, et, kt, ut;

    if (GetThreadTimes(t->handle, &ct, &et, &kt, &ut)) {
        ULONGLONG total = (((ULONGLONG)kt.dwHighDateTime << 32) | kt.dwLowDateTime)
                        + (((ULONGLONG)ut.dwHighDateTime << 32) | ut.dwLowDateTime);
        return (double)(total - t->cpu_100ns_at_start) / 1e7;
    }
    return 0.0;
}

#else  /* __APPLE__ */

static ULONGLONG thread_cpu_100ns(thread_act_t port)
{
    thread_basic_info_data_t info;
    mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;

    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &n) != KERN_SUCCESS)
        return 0;
    return (ULONGLONG)(info.user_time.seconds + info.system_time.seconds) * 10000000ull
         + (ULONGLONG)(info.user_time.microseconds + info.system_time.microseconds) * 10ull;
}

static double thread_cpu_seconds(const SampleThread *t)
{
    ULONGLONG now = thread_cpu_100ns(t->handle);
    return now ? (double)(now - t->cpu_100ns_at_start) / 1e7 : 0.0;
}

static void thread_name(SampleThread *t)
{
    pthread_t pt;

    if (t->tid == s_main_tid) {
        strcpy(t->name, "guest main");
        return;
    }
    pt = pthread_from_mach_thread_np(t->handle);
    if (pt)
        pthread_getname_np(pt, t->name, sizeof t->name);
    if (!t->name[0])
        snprintf(t->name, sizeof t->name, "thread %lu", (unsigned long)t->tid);
}

/* Keep the thread table current: threads come and go. Once a second. A
 * thread that has gone keeps its row (and the port right) so its samples
 * still report. */
static void refresh_threads(void)
{
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0, k;
    int i;

    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS)
        return;
    for (i = 0; i < s_thread_count; i++)
        s_threads[i].alive = 0;

    for (k = 0; k < count; k++) {
        thread_act_t port = list[k];
        int keep = 0;

        if (port == (thread_act_t)s_self_tid)
            goto drop;
        for (i = 0; i < s_thread_count; i++)
            if (s_threads[i].handle == port)
                break;
        if (i < s_thread_count) {
            s_threads[i].alive = 1;
            goto drop;          /* the row already holds a right */
        }
        if (s_thread_count < SAMPLE_MAX_THREADS) {
            SampleThread *t = &s_threads[s_thread_count];
            memset(t, 0, sizeof *t);
            t->tid = (DWORD)port;
            t->handle = port;
            t->alive = 1;
            t->leaf = (SampleSlot *)calloc(SAMPLE_SLOTS, sizeof(SampleSlot));
            t->incl = (SampleSlot *)calloc(SAMPLE_SLOTS, sizeof(SampleSlot));
            t->chain = (SampleChain *)calloc(SAMPLE_CHAIN_SLOTS, sizeof(SampleChain));
            if (t->leaf && t->incl && t->chain) {
                t->cpu_100ns_at_start = thread_cpu_100ns(port);
                thread_name(t);
                s_thread_count++;
                keep = 1;
            } else {
                free(t->leaf); free(t->incl); free(t->chain);
            }
        }
    drop:
        if (!keep)
            mach_port_deallocate(mach_task_self(), port);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof *list);
}

/* Read one thread's pc and walk its frame records. Nothing here allocates or
 * takes a lock, because the suspended thread may hold malloc's. */
static int sample_thread(SampleThread *t, uintptr_t *frames, int max_frames)
{
    uintptr_t pc, fp;
    int n = 0;

    if (thread_suspend(t->handle) != KERN_SUCCESS) {
        t->alive = 0;
        return 0;
    }
#if defined(__aarch64__)
    {
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        uintptr_t lr, rec[2];
        uint32_t insn = 0;
        mach_vm_size_t got = 0;

        if (thread_get_state(t->handle, ARM_THREAD_STATE64, (thread_state_t)&st,
                             &cnt) != KERN_SUCCESS)
            goto out;
        pc = (uintptr_t)arm_thread_state64_get_pc(st);
        fp = (uintptr_t)arm_thread_state64_get_fp(st);
        lr = (uintptr_t)arm_thread_state64_get_lr(st) & 0x0000FFFFFFFFFFFFull;

        /* A leaf function keeps no frame record (Apple's arm64 default), so
         * its caller is only in lr and the fp walk would skip it. Take lr as
         * the first caller when it is a return address (the instruction
         * before it is a BL or BLR) that the first frame record does not
         * already hold. Inside a non-leaf, after one of its own calls, lr
         * points back into itself and the function is counted twice in the
         * inclusive view; that is the price of seeing a leaf's caller. */
        frames[n++] = pc;
        if (lr > 4 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(lr - 4),
                                             sizeof insn, (mach_vm_address_t)&insn, &got)
                == KERN_SUCCESS && got == sizeof insn
            && ((insn & 0xFC000000u) == 0x94000000u            /* bl  */
                || (insn & 0xFFFFFC1Fu) == 0xD63F0000u)) {      /* blr */
            got = 0;
            if (!(fp && !(fp & 7) &&
                  mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)fp,
                                         sizeof rec, (mach_vm_address_t)rec, &got)
                      == KERN_SUCCESS && got == sizeof rec &&
                  (rec[1] & 0x0000FFFFFFFFFFFFull) == lr) && n < max_frames)
                frames[n++] = lr;
        }
        goto walk;
    }
#else
    {
        x86_thread_state64_t st;
        mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
        if (thread_get_state(t->handle, x86_THREAD_STATE64, (thread_state_t)&st,
                             &cnt) != KERN_SUCCESS)
            goto out;
        pc = (uintptr_t)st.__rip;
        fp = (uintptr_t)st.__rbp;
    }
    frames[n++] = pc;
    goto walk;
#endif
walk:
    while (n < max_frames && fp && !(fp & 7)) {
        uintptr_t rec[2];            /* [0] caller's fp, [1] return address */
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)fp,
                                   sizeof rec, (mach_vm_address_t)rec, &got)
                != KERN_SUCCESS || got != sizeof rec)
            break;
        /* Return addresses carry no pointer-authentication bits in an arm64
         * (not arm64e) process; mask to the user address range anyway. */
        rec[1] &= 0x0000FFFFFFFFFFFFull;
        if (!rec[1])
            break;
        frames[n++] = rec[1];
        if (rec[0] <= fp)            /* stacks grow down: callers are above */
            break;
        fp = rec[0];
    }
out:
    thread_resume(t->handle);
    return n;
}
#endif /* __APPLE__ */

/* ------------------------------------------------------------ resolving */

typedef struct {
    uintptr_t addr;          /* the sampled address */
    uintptr_t base;          /* symbol start, for grouping */
    char      name[96];
    char      module[32];
} ResolvedAddr;

static ResolvedAddr *s_resolved;
static unsigned s_resolved_used;
#define RESOLVE_SLOTS (1u << 17)

static const ResolvedAddr *resolve(uintptr_t addr)
{
    unsigned i = (unsigned)((addr >> 2) * 2654435761u) & (RESOLVE_SLOTS - 1);
    unsigned n;

    if (!s_resolved) {
        s_resolved = (ResolvedAddr *)calloc(RESOLVE_SLOTS, sizeof(ResolvedAddr));
        if (!s_resolved)
            return NULL;
    }
    for (n = 0; n < RESOLVE_SLOTS; n++, i = (i + 1) & (RESOLVE_SLOTS - 1)) {
        ResolvedAddr *r = &s_resolved[i];
        if (r->addr == addr)
            return r;
        if (!r->addr) {
#ifndef _WIN32
            Dl_info info;
            const char *base = "?";

            r->addr = addr;
            r->base = addr;
            s_resolved_used++;
            memset(&info, 0, sizeof info);
            if (dladdr((const void *)addr, &info) && info.dli_fname) {
                const char *p;
                base = info.dli_fname;
                for (p = info.dli_fname; *p; p++)
                    if (*p == '/')
                        base = p + 1;
            }
            snprintf(r->module, sizeof r->module, "%s", base);
            if (info.dli_sname && info.dli_saddr) {
                snprintf(r->name, sizeof r->name, "%s", info.dli_sname);
                r->base = (uintptr_t)info.dli_saddr;
            } else {
                snprintf(r->name, sizeof r->name, "%s+0x%llx", r->module,
                         (unsigned long long)(addr - (uintptr_t)info.dli_fbase));
                r->base = (uintptr_t)info.dli_fbase;
            }
            return r;
#else
            char buf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
            DWORD64 disp = 0;
            HMODULE mod = NULL;

            r->addr = addr;
            r->base = addr;
            s_resolved_used++;
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCWSTR)addr, &mod) && mod) {
                char path[MAX_PATH];
                DWORD len = GetModuleFileNameA(mod, path, MAX_PATH);
                const char *base = path;
                if (len) {
                    const char *p;
                    for (p = path; *p; p++)
                        if (*p == '\\' || *p == '/')
                            base = p + 1;
                }
                snprintf(r->module, sizeof r->module, "%s", len ? base : "?");
            } else {
                strcpy(r->module, "?");
            }
            memset(buf, 0, sizeof buf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(GetCurrentProcess(), (DWORD64)addr, &disp, sym)) {
                snprintf(r->name, sizeof r->name, "%s", sym->Name);
                r->base = (uintptr_t)sym->Address;
            } else {
                snprintf(r->name, sizeof r->name, "%s+0x%llx", r->module,
                         (unsigned long long)(addr - (uintptr_t)mod));
                r->base = (uintptr_t)mod;   /* one bucket per unresolved module */
            }
            return r;
#endif
        }
    }
    return NULL;
}

/* Where a sample's time belongs, from its name and module. */
enum {
    CAT_LIFTED, CAT_TRACE, CAT_RUNTIME, CAT_HLE, CAT_D3D_TRANSLATE, CAT_KERNEL,
    CAT_PUSHBUFFER, CAT_APU, CAT_CRT, CAT_HOST_D3D, CAT_WAIT, CAT_OS, CAT_OTHER,
    CAT_COUNT
};
static const char *const CAT_NAMES[CAT_COUNT] = {
    "lifted game code", "trace hook (recomp_trace_enter)", "runtime (dispatch, icall)",
    "hle_ (HLE boundary)", "d3d8_ (host renderer layer)", "kernel bridge",
    "nv2a_ (push buffer)", "apu / dsound", "C runtime",
#ifdef _WIN32
    "host D3D11/driver",
#else
    "host Metal/MoltenVK/driver",
#endif
    "waiting (kernel wait/sleep)", "OS (other)", "other"
};

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int categorise(const ResolvedAddr *r)
{
    const char *m = r->module, *n = r->name;

#ifndef _WIN32
    /* Darwin: every blocking call ends in a libsystem trap (and a sample can
     * land in the libc or pthread wrapper around it). */
    if (starts(m, "libsystem_")
        && (strstr(n, "wait") || strstr(n, "psynch") || strstr(n, "ulock")
            || strstr(n, "mach_msg") || strstr(n, "sleep") || strstr(n, "kevent")
            || strstr(n, "select") || strstr(n, "poll") || strstr(n, "workq")
            || strstr(n, "semwait") || strstr(n, "os_sync")))
        return CAT_WAIT;
    if (strcmp(m, "libsystem_kernel.dylib") == 0)
        return CAT_OS;
    if (starts(m, "libsystem_c") || starts(m, "libsystem_malloc")
        || starts(m, "libsystem_platform") || starts(m, "libc++"))
        return CAT_CRT;
    if (starts(m, "libsystem_") || starts(m, "libdyld") || starts(m, "libobjc")
        || starts(m, "CoreFoundation") || starts(m, "Foundation"))
        return CAT_OS;
    if (starts(m, "Metal") || starts(m, "AGX") || starts(m, "libMoltenVK")
        || starts(m, "libvulkan") || starts(m, "IOGPU") || starts(m, "QuartzCore"))
        return CAT_HOST_D3D;
#endif
    if (_stricmp(m, "ntdll.dll") == 0 || _stricmp(m, "KERNELBASE.dll") == 0
        || _stricmp(m, "kernel32.dll") == 0 || _stricmp(m, "win32u.dll") == 0) {
        if (strstr(n, "Wait") || strstr(n, "Delay") || strstr(n, "Sleep")
            || strstr(n, "Yield") || strstr(n, "RemoveIoCompletion")
            || strstr(n, "SignalAndWait") || strstr(n, "GetMessage")
            || strstr(n, "ReplyWaitReceive") || strstr(n, "AlertByThreadId"))
            return CAT_WAIT;
        return CAT_OS;
    }
    /* A message loop parked in the window manager is a wait too. */
    if (_stricmp(m, "user32.dll") == 0 && (strstr(n, "GetMessage")
                                            || strstr(n, "MsgWait")))
        return CAT_WAIT;
    if (starts(m, "d3d11") || starts(m, "dxgi") || starts(m, "D3DCompiler")
        || starts(m, "nvwgf") || starts(m, "nvldumd") || starts(m, "igd")
        || starts(m, "amdxc") || starts(m, "atid") || starts(m, "D3DSCache")
        || starts(m, "dxcore") || starts(m, "d3d10warp") || starts(m, "d3d12"))
        return CAT_HOST_D3D;
    if (starts(m, "ucrtbase") || starts(m, "vcruntime") || starts(m, "msvcp")
        || starts(m, "msvcrt"))
        return CAT_CRT;

    if (s_exe && (uintptr_t)r->addr >= s_exe_lo && (uintptr_t)r->addr < s_exe_hi) {
        if (starts(n, "sub_") || starts(n, "xbe_entry")) return CAT_LIFTED;
        if (starts(n, "recomp_trace") || starts(n, "prof_") || starts(n, "watch_check")
            || starts(n, "dump_va_once") || starts(n, "trace_only")) return CAT_TRACE;
        if (starts(n, "recomp_")) return CAT_RUNTIME;
        if (starts(n, "hle_")) return CAT_HLE;
        if (starts(n, "d3d8_") || starts(n, "host_") || starts(n, "vsh_")
            || starts(n, "combiner")) return CAT_D3D_TRANSLATE;
        if (starts(n, "nv2a_")) return CAT_PUSHBUFFER;
        if (starts(n, "mcpx_") || starts(n, "apu_") || starts(n, "dsp_")
            || starts(n, "dsound")) return CAT_APU;
        if (starts(n, "bridge_") || starts(n, "xbox_") || starts(n, "kernel_")
            || starts(n, "ke_") || starts(n, "Ke") || starts(n, "Nt")
            || starts(n, "Rtl") || starts(n, "Ps") || starts(n, "Io")
            || starts(n, "Mm") || starts(n, "Hal") || starts(n, "Ex"))
            return CAT_KERNEL;
        if (starts(n, "memcpy") || starts(n, "memset") || starts(n, "memmove")
            || starts(n, "strlen") || starts(n, "getenv") || starts(n, "_"))
            return CAT_CRT;
        return CAT_OTHER;
    }
    return CAT_OTHER;
}

/* --------------------------------------------------------------- report */

typedef struct { uintptr_t base; unsigned count; const ResolvedAddr *r; } Agg;

static int agg_cmp(const void *a, const void *b)
{
    unsigned ca = ((const Agg *)a)->count, cb = ((const Agg *)b)->count;
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

/* Fold a per-address table into per-symbol counts, hottest first. */
static int aggregate(const SampleSlot *table, Agg *out, int max_out,
                     unsigned cats[CAT_COUNT])
{
    int n = 0;
    unsigned i;

    for (i = 0; i < SAMPLE_SLOTS; i++) {
        const ResolvedAddr *r;
        int j;

        if (!table[i].addr)
            continue;
        r = resolve(table[i].addr);
        if (!r)
            continue;
        if (cats)
            cats[categorise(r)] += table[i].count;
        for (j = 0; j < n; j++)
            if (out[j].base == r->base) { out[j].count += table[i].count; break; }
        if (j == n && n < max_out) {
            out[n].base = r->base;
            out[n].count = table[i].count;
            out[n].r = r;
            n++;
        }
    }
    qsort(out, (size_t)n, sizeof *out, agg_cmp);
    return n;
}

#define AGG_MAX 40000

/* A chain folded to symbol bases, so two samples at different instructions
 * of the same four functions count together. */
typedef struct {
    uintptr_t b[SAMPLE_CHAIN];
    const ResolvedAddr *r[SAMPLE_CHAIN];
    unsigned count;
} ChainAgg;
#define CHAIN_AGG_SLOTS (1u << 14)

static int chain_cmp(const void *a, const void *b)
{
    unsigned ca = ((const ChainAgg *)a)->count, cb = ((const ChainAgg *)b)->count;
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}

/* For the hottest leaves of the thread's exclusive table (agg, n entries,
 * hottest first), the callers each was sampled under. Three chains per leaf,
 * the first twelve leaves that are not a wait. */
static void report_chains(const SampleThread *t, const Agg *agg, int n,
                          unsigned running, FILE *dump)
{
    static ChainAgg *table;
    unsigned i, k, used = 0;
    int shown_leaves = 0, k2;

    if (!running || !t->chain)
        return;
    if (!table)
        table = (ChainAgg *)calloc(CHAIN_AGG_SLOTS, sizeof *table);
    if (!table)
        return;
    memset(table, 0, CHAIN_AGG_SLOTS * sizeof *table);

    /* Fold the per-address chains to per-symbol ones. */
    for (i = 0; i < SAMPLE_CHAIN_SLOTS; i++) {
        const SampleChain *c = &t->chain[i];
        uintptr_t b[SAMPLE_CHAIN];
        const ResolvedAddr *r[SAMPLE_CHAIN];
        unsigned h = 0, j, m;

        if (!c->count)
            continue;
        for (k = 0; k < SAMPLE_CHAIN; k++) {
            r[k] = c->f[k] ? resolve(c->f[k]) : NULL;
            b[k] = r[k] ? r[k]->base : c->f[k];
            h = h * 2654435761u + (unsigned)(b[k] >> 2);
        }
        if (!r[0])
            continue;
        j = h & (CHAIN_AGG_SLOTS - 1);
        for (m = 0; m < CHAIN_AGG_SLOTS; m++, j = (j + 1) & (CHAIN_AGG_SLOTS - 1)) {
            if (table[j].count && memcmp(table[j].b, b, sizeof b) == 0) {
                table[j].count += c->count;
                break;
            }
            if (!table[j].count) {
                memcpy(table[j].b, b, sizeof b);
                memcpy(table[j].r, r, sizeof r);
                table[j].count = c->count;
                used++;
                break;
            }
        }
    }
    if (!used)
        return;
    /* Compact and sort, hottest first. */
    {
        unsigned w = 0;
        for (i = 0; i < CHAIN_AGG_SLOTS; i++)
            if (table[i].count)
                table[w++] = table[i];
        qsort(table, w, sizeof *table, chain_cmp);
        used = w;
    }

    fprintf(stderr, "      hottest leaves, by caller:\n");
    if (dump)
        fprintf(dump, "# chain\tsamples\tleaf\tcallers, nearest first\n");
    for (k2 = 0; k2 < n && shown_leaves < 12; k2++) {
        int shown = 0;

        if (categorise(agg[k2].r) == CAT_WAIT)
            continue;
        shown_leaves++;
        for (i = 0; i < used && shown < 3; i++) {
            const ChainAgg *c = &table[i];
            int d;

            if (c->b[0] != agg[k2].base)
                continue;
            fprintf(stderr, "      %6.2f%%  %s", 100.0 * c->count / (double)running,
                    c->r[0]->name);
            for (d = 1; d < SAMPLE_CHAIN; d++) {
                if (!c->b[d])
                    break;
                fprintf(stderr, " <- %s", c->r[d] ? c->r[d]->name : "?");
            }
            fprintf(stderr, "\n");
            shown++;
        }
    }
    if (dump) {
        for (i = 0; i < used; i++) {
            const ChainAgg *c = &table[i];
            int d;

            fprintf(dump, "chain\t%u", c->count);
            for (d = 0; d < SAMPLE_CHAIN; d++)
                fprintf(dump, "\t%s", c->b[d] ? (c->r[d] ? c->r[d]->name : "?") : "");
            fprintf(dump, "\n");
        }
    }
}

static void report(int final)
{
    static Agg *agg;
    LARGE_INTEGER now;
    double elapsed;
    const char *dump_path = getenv("RECOMP_SAMPLE_DUMP");
    FILE *dump = NULL;
    int i;
    unsigned total_running = 0;

    if (!agg)
        agg = (Agg *)calloc(AGG_MAX, sizeof *agg);
    if (!agg)
        return;
    QueryPerformanceCounter(&now);
    elapsed = (double)(now.QuadPart - s_t0.QuadPart) / (double)s_qpf.QuadPart;

    if (dump_path && *dump_path)
        dump = fopen(dump_path, "w");

    EnterCriticalSection(&s_lock);
    fprintf(stderr, "\n[SAMPLE] %s after %.0fs: %llu samples at %.0f Hz, %d threads\n",
            final ? "final" : "report", elapsed, s_total_samples, s_hz, s_thread_count);
    if (dump)
        fprintf(dump, "# sampling profile after %.0fs: %llu samples at %.0f Hz\n",
                elapsed, s_total_samples, s_hz);

    /* Per thread: how much of the run it was on-CPU, and where. */
    for (i = 0; i < s_thread_count; i++) {
        SampleThread *t = &s_threads[i];
        unsigned cats[CAT_COUNT], running, c;
        double cpu_s;
        int n, k;

        if (!t->samples)
            continue;
        cpu_s = thread_cpu_seconds(t);
        memset(cats, 0, sizeof cats);
        n = aggregate(t->leaf, agg, AGG_MAX, cats);
        running = t->samples - cats[CAT_WAIT];
        total_running += running;

        /* Anonymous threads that only ever wait are not worth a paragraph.
         * The runtime's own threads are always shown: "the timer thread used
         * 0.3 s of CPU" is an answer about the ISR chain's cost, and its
         * absence would read as the thread not existing. */
        if (running * 100u < t->samples && cpu_s < 0.5
            && strncmp(t->name, "thread ", 7) == 0)
            continue;

        fprintf(stderr, "  [%-22s tid %-6lu] %7u samples, %5.1f%% on-CPU, %.1fs CPU time\n",
                t->name, (unsigned long)t->tid, t->samples,
                100.0 * running / (double)t->samples, cpu_s);
        for (c = 0; c < CAT_COUNT; c++) {
            if (!cats[c] || c == CAT_WAIT)
                continue;
            fprintf(stderr, "      %5.1f%% of on-CPU  %s\n",
                    running ? 100.0 * cats[c] / (double)running : 0.0, CAT_NAMES[c]);
        }
        fprintf(stderr, "      leaf (exclusive), hottest first:\n");
        for (k = 0; k < n && k < 24; k++) {
            if (categorise(agg[k].r) == CAT_WAIT)
                continue;
            fprintf(stderr, "      %6.2f%%  %s  [%s]\n",
                    running ? 100.0 * agg[k].count / (double)running : 0.0,
                    agg[k].r->name, agg[k].r->module);
        }
        if (dump) {
            fprintf(dump, "\n# thread %s tid %lu: %u samples, %u on-CPU, %.1fs CPU\n"
                          "# leaf\tsamples\tsymbol\tmodule\n",
                    t->name, (unsigned long)t->tid, t->samples, running, cpu_s);
            for (k = 0; k < n; k++)
                fprintf(dump, "leaf\t%u\t%s\t%s\n", agg[k].count,
                        agg[k].r->name, agg[k].r->module);
        }

        /* Who pays for each of the hottest leaves: the callers above it,
         * folded to symbols. A leaf in the C runtime or the driver is only
         * actionable through the toolkit function that called it. */
        report_chains(t, agg, n, running, dump);

        /* Inclusive: what the leaf time belongs to. */
        n = aggregate(t->incl, agg, AGG_MAX, NULL);
        fprintf(stderr, "      inclusive (on the stack), hottest first:\n");
        for (k = 0; k < n && k < 24; k++) {
            if (categorise(agg[k].r) == CAT_WAIT)
                continue;
            fprintf(stderr, "      %6.2f%%  %s  [%s]\n",
                    running ? 100.0 * agg[k].count / (double)running : 0.0,
                    agg[k].r->name, agg[k].r->module);
        }
        if (dump) {
            fprintf(dump, "# inclusive\tsamples\tsymbol\tmodule\n");
            for (k = 0; k < n; k++)
                fprintf(dump, "incl\t%u\t%s\t%s\n", agg[k].count,
                        agg[k].r->name, agg[k].r->module);
        }

        /* How concentrated is it? The count of lifted functions that account
         * for half and for nine tenths of the lifted leaf time is the number
         * that separates "one hot path" from "diffuse". */
        if (cats[CAT_LIFTED]) {
            unsigned acc = 0, half = 0, ninety = 0, lifted_funcs = 0;
            n = aggregate(t->leaf, agg, AGG_MAX, NULL);
            for (k = 0; k < n; k++) {
                if (categorise(agg[k].r) != CAT_LIFTED)
                    continue;
                lifted_funcs++;
                acc += agg[k].count;
                if (!half && acc * 2 >= cats[CAT_LIFTED]) half = lifted_funcs;
                if (!ninety && acc * 10 >= cats[CAT_LIFTED] * 9) ninety = lifted_funcs;
            }
            fprintf(stderr, "      lifted leaf time: %u functions sampled; half of it "
                            "is in %u, nine tenths in %u\n",
                    lifted_funcs, half, ninety);
        }
    }
    fprintf(stderr, "  on-CPU samples across all threads: %u (%.2f cores busy)\n",
            total_running,
            s_total_samples ? (double)total_running / ((double)elapsed * s_hz) : 0.0);
    fflush(stderr);
    LeaveCriticalSection(&s_lock);
    if (dump)
        fclose(dump);
}

static void report_at_exit(void) { report(1); }

/* --------------------------------------------------------------- driver */

static double s_start_secs;       /* RECOMP_SAMPLE_START */

static DWORD WINAPI sampler_thread(LPVOID unused)
{
#ifdef _WIN32
    HANDLE timer;
#endif
    LARGE_INTEGER last_refresh, last_report;
    uintptr_t frames[SAMPLE_MAX_DEPTH];

    (void)unused;
#ifdef _WIN32
    s_self_tid = GetCurrentThreadId();
#else
    s_self_tid = (DWORD)mach_thread_self();
    pthread_setname_np("sampler");
#endif
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_t0);
    last_refresh.QuadPart = 0;
    last_report = s_t0;

#ifdef _WIN32
    /* The high-resolution timer is what makes a 1 kHz rate real; without it
     * the wait rounds up to the scheduler tick. */
    timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */,
                                   TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, TRUE, NULL);
#endif

    for (;;) {
        LARGE_INTEGER now;
        int i;

#ifdef _WIN32
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(10000000.0 / s_hz);
            if (due.QuadPart > -1) due.QuadPart = -1;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
#else
        {
            /* A sampling interval need not be exact, only regular. */
            double ns = 1e9 / s_hz;
            struct timespec ts = { 0, (long)(ns < 1e5 ? 1e5 : ns) };
            nanosleep(&ts, NULL);
        }
#endif
        QueryPerformanceCounter(&now);
        if (s_start_secs > 0.0 &&
            (double)(now.QuadPart - s_t0.QuadPart) / (double)s_qpf.QuadPart < s_start_secs) {
            last_report = now;
            continue;
        }
        if (now.QuadPart - last_refresh.QuadPart > s_qpf.QuadPart) {
            refresh_threads();
            last_refresh = now;
        }

        EnterCriticalSection(&s_lock);
        for (i = 0; i < s_thread_count; i++) {
            SampleThread *t = &s_threads[i];
            int n, k, j;

            if (!t->alive)
                continue;
            n = sample_thread(t, frames, s_depth < SAMPLE_MAX_DEPTH ? s_depth : SAMPLE_MAX_DEPTH);
            if (!n)
                continue;
            t->samples++;
            slot_add(t->leaf, &t->leaf_used, frames[0]);
            chain_add(t->chain, &t->chain_used, frames, n);
            for (k = 0; k < n; k++) {
                for (j = 0; j < k; j++)
                    if (frames[j] == frames[k])
                        break;
                if (j == k)
                    slot_add(t->incl, &t->incl_used, frames[k]);
            }
        }
        s_total_samples++;
        LeaveCriticalSection(&s_lock);

        if ((double)(now.QuadPart - last_report.QuadPart) / (double)s_qpf.QuadPart
                >= s_report_secs) {
            last_report = now;
            report(0);
        }
    }
    return 0;
}

/* Start sampling if RECOMP_SAMPLE is set. Call from the thread that runs the
 * guest's main thread, which is then named as such in the report. */
void xbox_SamplerStart(void)
{
    const char *v = getenv("RECOMP_SAMPLE");
    HANDLE h;
#ifdef _WIN32
    MODULEINFO mi;
#endif

    if (!v || !*v)
        return;
    s_hz = atof(v);
    if (s_hz <= 0.0) s_hz = 1000.0;
    if (s_hz > 4000.0) s_hz = 4000.0;
    v = getenv("RECOMP_SAMPLE_REPORT");
    if (v && atof(v) > 0.0) s_report_secs = atof(v);
    v = getenv("RECOMP_SAMPLE_DEPTH");
    if (v && atoi(v) > 0) s_depth = atoi(v);
    v = getenv("RECOMP_SAMPLE_START");
    if (v && atof(v) > 0.0) s_start_secs = atof(v);

#ifdef _WIN32
    s_main_tid = GetCurrentThreadId();
    s_exe = GetModuleHandleW(NULL);
    if (GetModuleInformation(GetCurrentProcess(), s_exe, &mi, sizeof mi)) {
        s_exe_lo = (uintptr_t)mi.lpBaseOfDll;
        s_exe_hi = s_exe_lo + mi.SizeOfImage;
    }
    /* The host program normally initialises dbghelp for its crash handler;
     * doing it again is refused harmlessly. Without it, addresses resolve to
     * module+offset only. */
    SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
#else
    s_main_tid = (DWORD)mach_thread_self();
    {
        /* The executable is image 0; its code is the __TEXT segment. */
        const struct mach_header_64 *mh =
            (const struct mach_header_64 *)_dyld_get_image_header(0);
        const struct load_command *lc = (const struct load_command *)(mh + 1);
        uint32_t k;

        s_exe = (HMODULE)mh;
        for (k = 0; mh && k < mh->ncmds; k++) {
            if (lc->cmd == LC_SEGMENT_64 &&
                strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT") == 0) {
                s_exe_lo = (uintptr_t)mh;
                s_exe_hi = s_exe_lo + ((const struct segment_command_64 *)lc)->vmsize;
                break;
            }
            lc = (const struct load_command *)((const char *)lc + lc->cmdsize);
        }
    }
#endif

    InitializeCriticalSection(&s_lock);
    atexit(report_at_exit);
    h = CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    if (h) {
        SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(h);
    }
    fprintf(stderr, "  [SAMPLE] sampling every thread at %.0f Hz, %d frames deep, "
                    "reporting every %.0fs\n", s_hz, s_depth, s_report_secs);
}

/* Name a runtime thread for the report. Cheap, and harmless where the API is
 * missing. */
void xbox_NameCurrentThread(const wchar_t *name)
{
#ifdef _WIN32
    static SetThreadDescription_t set_desc;
    static int looked;

    if (!looked) {
        looked = 1;
        set_desc = (SetThreadDescription_t)(void *)GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription");
    }
    if (set_desc)
        set_desc(GetCurrentThread(), name);
#else
    /* Names are ASCII; Darwin keeps 63 bytes and names only the caller. */
    char buf[64];
    size_t i;

    for (i = 0; name && name[i] && i < sizeof buf - 1; i++)
        buf[i] = (name[i] < 0x80) ? (char)name[i] : '?';
    buf[i] = '\0';
    pthread_setname_np(buf);
#endif
}

#else  /* neither Windows nor macOS */

#include <wchar.h>

void xbox_SamplerStart(void) {}
void xbox_NameCurrentThread(const wchar_t *name) { (void)name; }

#endif
