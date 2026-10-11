/*
 * posix_memory.c - VirtualAlloc, VirtualProtect, VirtualQuery and file
 * mapping views on POSIX, with Windows' semantics. See posix_memory.h.
 */
#if !defined(_WIN32)

#define _GNU_SOURCE   /* memfd_create, MAP_FIXED_NOREPLACE */

#include "posix_memory.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_vm.h>
#endif

#ifndef ERROR_INVALID_ADDRESS
#define ERROR_INVALID_ADDRESS 487u
#endif
#ifndef ERROR_NOT_ENOUGH_MEMORY
#define ERROR_NOT_ENOUGH_MEMORY 8u
#endif
#ifndef ERROR_INVALID_PARAMETER
#define ERROR_INVALID_PARAMETER 87u
#endif

#define W32_MEM_FREE     0x10000u
#define W32_MEM_PRIVATE  0x20000u
#define W32_MEM_MAPPED   0x40000u

/* Guest pages are the Xbox's: 4 KB, whatever the host's are. */
#define GPAGE      ((uintptr_t)0x1000)
#define GPAGE_MASK (GPAGE - 1)

size_t w32_host_page_size(void)
{
    static size_t page;
    if (!page) {
        long p = sysconf(_SC_PAGESIZE);
        page = p > 0 ? (size_t)p : 4096;
    }
    return page;
}

static uintptr_t round_down(uintptr_t v, uintptr_t a) { return v & ~(a - 1); }
static uintptr_t round_up(uintptr_t v, uintptr_t a)   { return (v + a - 1) & ~(a - 1); }

/* Win32 protection, ranked by what it allows: 0 nothing, 1 read, 2 write.
 * Execute is irrelevant here -- nothing runs out of guest memory. */
static int prot_rank(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return 0;
    case PAGE_EXECUTE:           return 0;
    case PAGE_READONLY:          return 1;
    case PAGE_EXECUTE_READ:      return 1;
    case PAGE_READWRITE:         return 2;
    case PAGE_WRITECOPY:         return 2;
    case PAGE_EXECUTE_READWRITE: return 2;
    case 0x80u /* EXECUTE_WRITECOPY */: return 2;
    default:                     return 2;
    }
}

static int posix_prot(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_WRITECOPY:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_READ | PROT_WRITE;
    }
}

static DWORD win_prot(int rank)
{
    return rank >= 2 ? PAGE_READWRITE : rank == 1 ? PAGE_READONLY : PAGE_NOACCESS;
}

/* ===================================================================== */
/* Shared-memory objects                                                  */
/* ===================================================================== */

struct pm_object {
    int    fd;
    size_t size;
    void  *backdoor;     /* the whole object, read-write, somewhere else */
    LONG   refs;
};

static int anon_shared_fd(size_t size)
{
    int fd;
#if defined(__linux__)
    fd = memfd_create("xboxrecomp", MFD_CLOEXEC);
#else
    /* Darwin has no memfd. A POSIX shared-memory object unlinked the moment
     * it exists is the same thing: nameless, and gone when the last fd and
     * mapping are. The name only has to be unique for that instant. */
    static LONG counter;
    char name[32];
    int tries;

    fd = -1;
    for (tries = 0; tries < 16 && fd < 0; tries++) {
        snprintf(name, sizeof(name), "/xbr.%d.%ld", (int)getpid(),
                 (long)__atomic_add_fetch(&counter, 1, __ATOMIC_SEQ_CST));
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            shm_unlink(name);
        else if (errno != EEXIST)
            break;
    }
    if (fd >= 0)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

pm_object *pm_object_create(size_t size)
{
    pm_object *o;
    size_t     rounded = round_up(size, w32_host_page_size());

    if (!size)
        return NULL;
    o = (pm_object *)calloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->fd = anon_shared_fd(rounded);
    if (o->fd < 0) {
        free(o);
        return NULL;
    }
    o->size = rounded;
    o->refs = 1;
    /* The backdoor: one more view of the whole object, never protected, so
     * a fault handler can complete an access the guest's view refused
     * without touching that view's protection (no window in which another
     * thread sees the page writable). Address space only; no memory. */
    o->backdoor = mmap(NULL, rounded, PROT_READ | PROT_WRITE, MAP_SHARED, o->fd, 0);
    if (o->backdoor == MAP_FAILED) {
        close(o->fd);
        free(o);
        return NULL;
    }
    return o;
}

size_t pm_object_size(const pm_object *o) { return o ? o->size : 0; }

void pm_object_retain(pm_object *o)
{
    if (o)
        __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST);
}

void pm_object_release(pm_object *o)
{
    if (!o || __atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) > 0)
        return;
    munmap(o->backdoor, o->size);
    close(o->fd);
    free(o);
}

/* ===================================================================== */
/* The guest arena                                                        */
/* ===================================================================== */

/* What is placed in the arena. Append-only, so a fault handler can scan it
 * with no lock: a slot is filled in before the count that publishes it is
 * raised, and a removed placement is marked dead rather than reused. */
typedef struct {
    uintptr_t  lo, hi;
    pm_object *obj;
    size_t     off;           /* offset of lo within obj */
    DWORD      alloc_prot;    /* protection it was placed with */
    int        is_view;       /* MapViewOfFileEx (MEM_MAPPED), else VirtualAlloc */
    volatile int live;
} arena_place;

#define ARENA_MAX_PLACES 1024

static uintptr_t       s_arena_lo, s_arena_hi;
static uint8_t        *s_gprot;      /* PAGE_* per 4 KB page; 0 = nothing placed */
/* 1 where the page's protection is one the title asked for (w32_guest_protect)
 * rather than a trap the runtime set. See arena_apply_prot. */
static uint8_t        *s_gsoft;
static arena_place     s_places[ARENA_MAX_PLACES];
static volatile int    s_nplaces;
static pthread_mutex_t s_arena_lock = PTHREAD_MUTEX_INITIALIZER;

void *w32_reserve_arena(size_t size, size_t align)
{
    size_t page = w32_host_page_size();
    size_t span;
    uint8_t *raw;
    uintptr_t lo;

    if (s_arena_lo || !size)
        return NULL;
    if (align < page)
        align = page;
    size = round_up(size, page);
    span = size + align;
    raw = (uint8_t *)mmap(NULL, span, PROT_NONE,
                          MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (raw == MAP_FAILED)
        return NULL;
    lo = round_up((uintptr_t)raw, align);
    /* Give back the slack on either side, keeping exactly [lo, lo + size). */
    if (lo > (uintptr_t)raw)
        munmap(raw, lo - (uintptr_t)raw);
    if ((uintptr_t)raw + span > lo + size)
        munmap((void *)(lo + size), (uintptr_t)raw + span - (lo + size));

    s_gprot = (uint8_t *)calloc(size / GPAGE, 1);
    s_gsoft = (uint8_t *)calloc(size / GPAGE, 1);
    if (!s_gprot || !s_gsoft) {
        munmap((void *)lo, size);
        return NULL;
    }
    s_arena_lo = lo;
    s_arena_hi = lo + size;
    return (void *)lo;
}

int w32_in_arena(const void *addr)
{
    uintptr_t a = (uintptr_t)addr;
    return s_arena_lo && a >= s_arena_lo && a < s_arena_hi;
}

static int range_in_arena(uintptr_t lo, uintptr_t hi)
{
    return s_arena_lo && lo >= s_arena_lo && hi <= s_arena_hi && lo < hi;
}

DWORD w32_page_protection(const void *addr)
{
    if (!w32_in_arena(addr))
        return 0;
    return s_gprot[((uintptr_t)addr - s_arena_lo) / GPAGE];
}

static const arena_place *find_place(uintptr_t a)
{
    int n = __atomic_load_n(&s_nplaces, __ATOMIC_ACQUIRE);
    for (int i = n - 1; i >= 0; i--) {
        const arena_place *p = &s_places[i];
        if (p->live && a >= p->lo && a < p->hi)
            return p;
    }
    return NULL;
}

void *w32_backdoor(const void *addr)
{
    const arena_place *p;

    if (!w32_in_arena(addr))
        return NULL;
    p = find_place((uintptr_t)addr);
    if (!p)
        return NULL;
    return (uint8_t *)p->obj->backdoor + p->off + ((uintptr_t)addr - p->lo);
}

/* Bring every host page in [lo, hi) to the most restrictive protection of
 * the guest pages on it. Caller holds the arena lock.
 *
 * Except a protection the title asked for, on a host page it shares with a
 * writable guest page. The runtime's own traps (a device page, a watchpoint)
 * must fault exactly, and do: they take the host page down with them, and
 * the fault handler completes the accesses its neighbours were allowed. A
 * title's read-only or no-access page is different -- it only catches the
 * title's own stray writes -- and charging every access to the writable
 * pages beside it a host fault for that is out of all proportion: Halo 2
 * leaves read-only guard pages beside its movie buffer, and the 16 KB host
 * page made each of the decoder's writes a signal, 125,000 of them in 40 s,
 * on a movie that then never got a frame out. Such a page still traps when
 * every guest page on its host page is restricted too. */
static int arena_apply_prot(uintptr_t lo, uintptr_t hi)
{
    uintptr_t page = w32_host_page_size();
    uintptr_t hp;
    int ok = 1;

    for (hp = round_down(lo, page); hp < hi; hp += page) {
        int rank = 2, hard = 2, soft = 2, writable = 0;
        for (uintptr_t g = hp; g < hp + page; g += GPAGE) {
            size_t i = (g - s_arena_lo) / GPAGE;
            uint8_t gp = s_gprot[i];
            int r = gp ? prot_rank(gp) : 0;
            if (r == 2)
                writable = 1;
            if (gp && s_gsoft[i]) {
                if (r < soft)
                    soft = r;
            } else if (r < hard) {
                hard = r;
            }
        }
        rank = hard;
        if (!writable && soft < rank)
            rank = soft;
        if (mprotect((void *)hp, page, posix_prot(win_prot(rank))) != 0)
            ok = 0;
    }
    return ok;
}

/* Place obj[off, off + len) at exactly at, inside the arena. */
static void *arena_place_at(pm_object *obj, size_t off, uintptr_t at, size_t len,
                            DWORD protect, int is_view)
{
    uintptr_t page = w32_host_page_size();
    uintptr_t lo = at, hi;
    void *p;
    int slot;

    if ((at & (page - 1)) || (off & (page - 1))) {
        /* A host page cannot be split between two placements: the views
         * would overlap on the host even though the guest ranges do not. */
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    hi = round_up(at + len, page);
    if (!range_in_arena(lo, hi) || off + (hi - lo) > obj->size) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }

    pthread_mutex_lock(&s_arena_lock);
    for (uintptr_t g = lo; g < hi; g += GPAGE) {
        if (s_gprot[(g - s_arena_lo) / GPAGE]) {
            pthread_mutex_unlock(&s_arena_lock);
            SetLastError(ERROR_INVALID_ADDRESS);   /* taken: Windows fails too */
            return NULL;
        }
    }
    slot = s_nplaces;
    if (slot >= ARENA_MAX_PLACES) {
        static int said;
        pthread_mutex_unlock(&s_arena_lock);
        /* Slots are never reused (the fault path reads them unlocked), so
         * placements that churn run out here. Say so, or it reads as an
         * address that would not map. */
        if (!said++)
            fprintf(stderr, "[MEM] guest arena: all %d placement slots used; "
                    "placement at %p refused\n", ARENA_MAX_PLACES, (void *)lo);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    p = mmap((void *)lo, hi - lo, posix_prot(protect), MAP_SHARED | MAP_FIXED,
             obj->fd, (off_t)off);
    if (p == MAP_FAILED || (uintptr_t)p != lo) {
        pthread_mutex_unlock(&s_arena_lock);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    pm_object_retain(obj);
    s_places[slot].lo = lo;
    s_places[slot].hi = hi;
    s_places[slot].obj = obj;
    s_places[slot].off = off;
    s_places[slot].alloc_prot = protect;
    s_places[slot].is_view = is_view;
    s_places[slot].live = 1;
    memset(s_gprot + (lo - s_arena_lo) / GPAGE,
           (protect & 0xFF) ? (int)(protect & 0xFF) : PAGE_READWRITE,
           (hi - lo) / GPAGE);
    memset(s_gsoft + (lo - s_arena_lo) / GPAGE, 0, (hi - lo) / GPAGE);
    __atomic_store_n(&s_nplaces, slot + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s_arena_lock);
    return p;
}

/* Take a placement out again, leaving the range reserved. */
static int arena_unplace(uintptr_t at)
{
    int n, found = -1;

    pthread_mutex_lock(&s_arena_lock);
    n = s_nplaces;
    for (int i = 0; i < n; i++)
        if (s_places[i].live && s_places[i].lo == at) { found = i; break; }
    if (found < 0) {
        pthread_mutex_unlock(&s_arena_lock);
        return 0;
    }
    {
        arena_place *p = &s_places[found];
        p->live = 0;
        mmap((void *)p->lo, p->hi - p->lo, PROT_NONE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
        memset(s_gprot + (p->lo - s_arena_lo) / GPAGE, 0, (p->hi - p->lo) / GPAGE);
        memset(s_gsoft + (p->lo - s_arena_lo) / GPAGE, 0, (p->hi - p->lo) / GPAGE);
        pm_object_release(p->obj);
    }
    pthread_mutex_unlock(&s_arena_lock);
    return 1;
}

/* ===================================================================== */
/* Host allocations outside the arena                                     */
/* ===================================================================== */

/* VirtualFree(MEM_RELEASE) is passed no length and VirtualQuery has to name
 * an allocation's base, so what VirtualAlloc and MapViewOfFile hand out is
 * remembered. Never consulted from a signal handler. */
typedef struct { uintptr_t lo, hi; DWORD prot; int is_view; } host_region;

static host_region    *s_regions;
static size_t          s_nregions, s_capregions;
static pthread_mutex_t s_regions_lock = PTHREAD_MUTEX_INITIALIZER;

static void region_add(void *p, size_t len, DWORD prot, int is_view)
{
    pthread_mutex_lock(&s_regions_lock);
    if (s_nregions == s_capregions) {
        size_t cap = s_capregions ? s_capregions * 2 : 64;
        host_region *r = (host_region *)realloc(s_regions, cap * sizeof(*r));
        if (!r) { pthread_mutex_unlock(&s_regions_lock); return; }
        s_regions = r;
        s_capregions = cap;
    }
    s_regions[s_nregions].lo = (uintptr_t)p;
    s_regions[s_nregions].hi = (uintptr_t)p + len;
    s_regions[s_nregions].prot = prot;
    s_regions[s_nregions].is_view = is_view;
    s_nregions++;
    pthread_mutex_unlock(&s_regions_lock);
}

/* Copy out the region containing a; with remove set, also forget it if it
 * starts exactly at a. 1 if found. */
static int region_find(uintptr_t a, host_region *out, int remove)
{
    int found = 0;
    pthread_mutex_lock(&s_regions_lock);
    for (size_t i = 0; i < s_nregions; i++) {
        if (a >= s_regions[i].lo && a < s_regions[i].hi) {
            if (remove && s_regions[i].lo != a)
                break;
            *out = s_regions[i];
            if (remove)
                s_regions[i] = s_regions[--s_nregions];
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_regions_lock);
    return found;
}

static void region_set_prot(uintptr_t a, DWORD prot)
{
    pthread_mutex_lock(&s_regions_lock);
    for (size_t i = 0; i < s_nregions; i++)
        if (a >= s_regions[i].lo && a < s_regions[i].hi) {
            s_regions[i].prot = prot;
            break;
        }
    pthread_mutex_unlock(&s_regions_lock);
}

/* mmap at exactly `at`, or fail without disturbing what is there. */
static void *map_noreplace(void *at, size_t len, int prot, int flags, int fd, off_t off)
{
    void *p;
#if defined(MAP_FIXED_NOREPLACE)
    if (at) flags |= MAP_FIXED_NOREPLACE;
#endif
    /* Without MAP_FIXED_NOREPLACE the address is a hint: Darwin honours a
     * hint whose range is free and picks elsewhere when it is not, and the
     * check below turns "elsewhere" into the failure Windows would report.
     * Never bare MAP_FIXED, which would silently replace a live mapping. */
    p = mmap(at, len, prot, flags, fd, off);
    if (p == MAP_FAILED)
        return NULL;
    if (at && p != at) {
        munmap(p, len);
        errno = EEXIST;
        return NULL;
    }
    return p;
}

/* ===================================================================== */
/* Win32 entry points                                                     */
/* ===================================================================== */

void *pm_map_view(pm_object *o, size_t off, size_t len, int writable, void *at)
{
    DWORD prot = writable ? PAGE_READWRITE : PAGE_READONLY;
    void *p;

    if (!o || off > o->size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!len)
        len = o->size - off;
    if (off + len > o->size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (at && w32_in_arena(at))
        return arena_place_at(o, off, (uintptr_t)at, len, prot, 1);

    p = map_noreplace(at, len, posix_prot(prot), MAP_SHARED, o->fd, (off_t)off);
    if (!p) {
        SetLastError(at ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    /* No reference taken: a mapping keeps the shared memory alive by itself,
     * and only arena placements use the object's backdoor. */
    region_add(p, len, prot, 1);
    return p;
}

int pm_unmap_view(const void *addr)
{
    host_region r;

    if (w32_in_arena(addr))
        return arena_unplace((uintptr_t)addr);
    if (!region_find((uintptr_t)addr, &r, 1) || !r.is_view)
        return 0;
    return munmap((void *)r.lo, r.hi - r.lo) == 0;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    size_t page = w32_host_page_size();
    void *p;

    if (!size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    if (!(protect & 0xFF))
        protect = PAGE_READWRITE;

    if (address && w32_in_arena(address)) {
        uintptr_t lo = round_down((uintptr_t)address, GPAGE);
        DWORD old;

        /* Committing inside something already placed: a protection change,
         * as on Windows. */
        if (!(allocationType & MEM_RESERVE) && w32_page_protection((void *)lo)) {
            if (VirtualProtect((void *)lo, (uintptr_t)address + size - lo, protect, &old))
                return (void *)lo;
            return NULL;
        }
        {
            pm_object *o;
            uintptr_t plo = round_down((uintptr_t)address, page);
            size_t len = round_up((uintptr_t)address + size, page) - plo;

            o = pm_object_create(len);
            if (!o) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return NULL;
            }
            /* A reservation without a commit is inaccessible until committed. */
            p = arena_place_at(o, 0, plo, len,
                               (allocationType & MEM_COMMIT) ? protect : PAGE_NOACCESS, 0);
            pm_object_release(o);  /* the placement holds its own reference */
            return p;
        }
    }

    /* MEM_COMMIT on a region an earlier VirtualAlloc reserved: just adjust
     * the protection. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address) {
        uintptr_t lo = round_down((uintptr_t)address, page);
        uintptr_t hi = round_up((uintptr_t)address + size, page);
        if (mprotect((void *)lo, hi - lo, posix_prot(protect)) == 0) {
            region_set_prot(lo, protect);
            return (void *)lo;
        }
        /* fall through to a fresh mapping */
    }

    size = round_up(size, page);
    p = map_noreplace(address, size,
                      (allocationType & MEM_COMMIT) || !(allocationType & MEM_RESERVE)
                          ? posix_prot(protect) : PROT_NONE,
                      MAP_PRIVATE | MAP_ANON, -1, 0);
    if (!p) {
        SetLastError(address ? ERROR_INVALID_ADDRESS : ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    region_add(p, size, protect, 0);
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    size_t page = w32_host_page_size();
    host_region r;

    if (!address)
        return FALSE;

    if (w32_in_arena(address)) {
        if (freeType & MEM_RELEASE)
            return arena_unplace((uintptr_t)address) ? TRUE : FALSE;
        if (freeType & MEM_DECOMMIT) {
            DWORD old;
            return VirtualProtect(address, size ? size : GPAGE, PAGE_NOACCESS, &old);
        }
        return TRUE;
    }

    if (freeType & MEM_RELEASE) {
        /* Win32 passes size 0 and means the whole allocation. */
        if (region_find((uintptr_t)address, &r, 1))
            return munmap((void *)r.lo, r.hi - r.lo) == 0;
        if (size == 0)
            return TRUE;          /* not ours; the old behaviour */
        return munmap(address, round_up(size, page)) == 0;
    }
    if (freeType & MEM_DECOMMIT) {
        uintptr_t lo = round_down((uintptr_t)address, page);
        uintptr_t hi = round_up((uintptr_t)address + size, page);
        madvise((void *)lo, hi - lo, MADV_DONTNEED);
        return mprotect((void *)lo, hi - lo, PROT_NONE) == 0;
    }
    return TRUE;
}

static BOOL protect_pages(LPVOID address, SIZE_T size, DWORD newProtect,
                          PDWORD oldProtect, int soft);

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    return protect_pages(address, size, newProtect, oldProtect, 0);
}

BOOL w32_guest_protect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    return protect_pages(address, size, newProtect, oldProtect, 1);
}

static BOOL protect_pages(LPVOID address, SIZE_T size, DWORD newProtect,
                          PDWORD oldProtect, int soft)
{
    size_t page = w32_host_page_size();

    if (!size) size = 1;

    if (w32_in_arena(address)) {
        uintptr_t lo = round_down((uintptr_t)address, GPAGE);
        uintptr_t hi = round_up((uintptr_t)address + size, GPAGE);
        int ok;

        if (!range_in_arena(lo, hi)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        pthread_mutex_lock(&s_arena_lock);
        for (uintptr_t g = lo; g < hi; g += GPAGE) {
            if (!s_gprot[(g - s_arena_lo) / GPAGE]) {
                pthread_mutex_unlock(&s_arena_lock);
                SetLastError(ERROR_INVALID_ADDRESS);  /* nothing placed there */
                return FALSE;
            }
        }
        if (oldProtect)
            *oldProtect = s_gprot[(lo - s_arena_lo) / GPAGE];
        memset(s_gprot + (lo - s_arena_lo) / GPAGE,
               (newProtect & 0xFF) ? (int)(newProtect & 0xFF) : PAGE_NOACCESS,
               (hi - lo) / GPAGE);
        memset(s_gsoft + (lo - s_arena_lo) / GPAGE, soft ? 1 : 0, (hi - lo) / GPAGE);
        ok = arena_apply_prot(lo, hi);
        pthread_mutex_unlock(&s_arena_lock);
        return ok ? TRUE : FALSE;
    }

    {
        uintptr_t lo = round_down((uintptr_t)address, page);
        uintptr_t hi = round_up((uintptr_t)address + size, page);
        host_region r;

        if (oldProtect)
            *oldProtect = region_find(lo, &r, 0) ? r.prot : PAGE_READWRITE;
        if (mprotect((void *)lo, hi - lo, posix_prot(newProtect)) != 0) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return FALSE;
        }
        region_set_prot(lo, newProtect);
        return TRUE;
    }
}

SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    uintptr_t a = (uintptr_t)address;
    size_t page = w32_host_page_size();
    host_region r;

    if (!buffer || length < sizeof(*buffer))
        return 0;
    memset(buffer, 0, sizeof(*buffer));

    if (w32_in_arena(address)) {
        uintptr_t lo = round_down(a, GPAGE), g;
        uint8_t prot = s_gprot[(lo - s_arena_lo) / GPAGE];
        const arena_place *p = prot ? find_place(lo) : NULL;
        uintptr_t limit = p ? p->hi : s_arena_hi;

        /* A free run ends at the next placement; finding that from the
         * placement list keeps this from walking up to 1 MB of side table. */
        if (!p) {
            int n = __atomic_load_n(&s_nplaces, __ATOMIC_ACQUIRE);
            for (int i = 0; i < n; i++)
                if (s_places[i].live && s_places[i].lo > lo && s_places[i].lo < limit)
                    limit = s_places[i].lo;
        }

        /* A run of guest pages with the same protection (or, unplaced, the
         * free run up to the next placement). */
        if (!p)
            g = limit;          /* free all the way to the next placement */
        else
            for (g = lo + GPAGE; g < limit; g += GPAGE)
                if (s_gprot[(g - s_arena_lo) / GPAGE] != prot)
                    break;
        buffer->BaseAddress = (PVOID)lo;
        buffer->RegionSize  = g - lo;
        if (p) {
            buffer->AllocationBase    = (PVOID)p->lo;
            buffer->AllocationProtect = p->alloc_prot;
            buffer->State   = MEM_COMMIT;
            buffer->Protect = prot;
            buffer->Type    = p->is_view ? W32_MEM_MAPPED : W32_MEM_PRIVATE;
        } else {
            buffer->State   = W32_MEM_FREE;
            buffer->Protect = PAGE_NOACCESS;
        }
        return sizeof(*buffer);
    }

    if (region_find(a, &r, 0)) {
        buffer->BaseAddress       = (PVOID)round_down(a, page);
        buffer->AllocationBase    = (PVOID)r.lo;
        buffer->AllocationProtect = r.prot;
        buffer->RegionSize        = r.hi - round_down(a, page);
        buffer->State             = MEM_COMMIT;
        buffer->Protect           = r.prot;
        buffer->Type              = r.is_view ? W32_MEM_MAPPED : W32_MEM_PRIVATE;
        return sizeof(*buffer);
    }

    /* Not something VirtualAlloc made -- malloc'd memory, a stack, the
     * image. AllocationBase stays NULL so callers that free what VirtualAlloc
     * made (MmFreeContiguousMemory) leave it to free(). */
    buffer->BaseAddress = (PVOID)round_down(a, page);
    buffer->RegionSize  = page;
    buffer->State       = MEM_COMMIT;
    buffer->Protect     = PAGE_READWRITE;
    buffer->Type        = W32_MEM_PRIVATE;
#if defined(__APPLE__)
    {
        mach_vm_address_t ra = (mach_vm_address_t)a;
        mach_vm_size_t    rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;

        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &count, &obj) == KERN_SUCCESS) {
            if (ra > a) {
                /* a is in a hole below the next region. */
                buffer->State      = W32_MEM_FREE;
                buffer->Protect    = PAGE_NOACCESS;
                buffer->RegionSize = (size_t)(ra - round_down(a, page));
            } else {
                int rank = (info.protection & VM_PROT_WRITE) ? 2
                         : (info.protection & VM_PROT_READ) ? 1 : 0;
                buffer->Protect    = win_prot(rank);
                buffer->RegionSize = (size_t)(ra + rs - round_down(a, page));
            }
        }
    }
#endif
    return sizeof(*buffer);
}

#endif /* !_WIN32 */
