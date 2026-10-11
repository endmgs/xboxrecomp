/*
 * host_main_posix.c - host_main.h on macOS and Linux.
 */
#ifndef _WIN32

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE     /* dladdr, dl_iterate_phdr */
#endif

#include "host_main.h"
#include "recomp_fault.h"

#include <dlfcn.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#endif

int host_exe_path(char *buf, size_t bytes)
{
#ifdef __APPLE__
    char raw[PATH_MAX];
    uint32_t n = (uint32_t)sizeof raw;

    if (_NSGetExecutablePath(raw, &n) != 0)
        return 0;
    if (!realpath(raw, buf)) {
        if (strlen(raw) >= bytes)
            return 0;
        strcpy(buf, raw);
    }
    return strlen(buf) < bytes;
#else
    ssize_t n = readlink("/proc/self/exe", buf, bytes - 1);

    if (n <= 0 || (size_t)n >= bytes - 1)
        return 0;
    buf[n] = '\0';
    return 1;
#endif
}

void host_path_dirname(char *path)
{
    char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
    char *slash = a > b ? a : b;

    if (slash)
        *slash = '\0';
}

int host_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

int host_app_bundle_dir(char *buf, size_t bytes)
{
#ifdef __APPLE__
    char exe[PATH_MAX];
    size_t n;
    static const char tail[] = ".app/Contents/MacOS";

    if (!host_exe_path(exe, sizeof exe))
        return 0;
    host_path_dirname(exe);                  /* <...>/X.app/Contents/MacOS */
    n = strlen(exe);
    if (n < sizeof tail || strcmp(exe + n - (sizeof tail - 1), tail) != 0)
        return 0;
    exe[n - (sizeof tail - 1) + 4] = '\0';   /* keep "<...>/X.app" */
    if (strlen(exe) >= bytes)
        return 0;
    strcpy(buf, exe);
    return 1;
#else
    (void)buf;
    (void)bytes;
    return 0;
#endif
}

/* A descriptor something will read: a terminal, a file, a pipe. Not one
 * that is closed or is /dev/null, which is what a Finder launch gets. */
static int fd_is_read(int fd)
{
    struct stat st;

    if (fstat(fd, &st) != 0)
        return 0;
    if (isatty(fd))
        return 1;
    return !S_ISCHR(st.st_mode);
}

void host_setup_output(char *log_path, size_t bytes)
{
    char exe[PATH_MAX];

    log_path[0] = '\0';
    if (fd_is_read(1) || fd_is_read(2))
        return;                     /* a terminal, or redirected: leave it */
    if (!host_exe_path(exe, sizeof exe))
        return;
    snprintf(log_path, bytes, "%s.log", exe);
    {
        /* Inside a .app: never into the bundle (it is signed, and may be
         * read-only), but where macOS keeps an application's logs. */
        char app[PATH_MAX], dir[PATH_MAX];
        const char *home = getenv("HOME");
        const char *name = strrchr(exe, '/');

        if (host_app_bundle_dir(app, sizeof app) && home && *home && name) {
            snprintf(dir, sizeof dir, "%s/Library/Logs", home);
            mkdir(dir, 0755);
            snprintf(dir, sizeof dir, "%s/Library/Logs/xboxrecomp", home);
            mkdir(dir, 0755);
            snprintf(log_path, bytes, "%s%s.log", dir, name);
        }
    }
    if (!freopen(log_path, "w", stderr)) {   /* read-only folder */
        log_path[0] = '\0';
        return;
    }
    freopen(log_path, "a", stdout);
}

void host_error_box(const char *title, const char *message)
{
    (void)title;
    (void)message;
}

int host_symbol_name(uintptr_t addr, char *name, size_t bytes, uintptr_t *offset)
{
    Dl_info info;

    if (!dladdr((const void *)addr, &info) || !info.dli_sname)
        return 0;
    snprintf(name, bytes, "%s", info.dli_sname);
    if (offset)
        *offset = addr - (uintptr_t)info.dli_saddr;
    return 1;
}

#ifdef __APPLE__
int host_module_range(uintptr_t *lo, uintptr_t *hi)
{
    const struct mach_header_64 *mh =
        (const struct mach_header_64 *)_dyld_get_image_header(0);
    intptr_t slide = _dyld_get_image_vmaddr_slide(0);
    const uint8_t *p;
    uint64_t min = UINT64_MAX, max = 0;
    uint32_t i;

    if (!mh || mh->magic != MH_MAGIC_64)
        return 0;
    p = (const uint8_t *)(mh + 1);
    for (i = 0; i < mh->ncmds; i++) {
        const struct load_command *lc = (const struct load_command *)p;
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *seg = (const struct segment_command_64 *)p;
            /* Code only: a data word that happens to point at a global
             * (g_xbox_mem_offset, say) is not a return address. */
            if (strcmp(seg->segname, SEG_TEXT) == 0 && seg->vmsize) {
                if (seg->vmaddr < min)
                    min = seg->vmaddr;
                if (seg->vmaddr + seg->vmsize > max)
                    max = seg->vmaddr + seg->vmsize;
            }
        }
        p += lc->cmdsize;
    }
    if (max <= min)
        return 0;
    *lo = (uintptr_t)(min + slide);
    *hi = (uintptr_t)(max + slide);
    return 1;
}
#else
static int first_object(struct dl_phdr_info *info, size_t size, void *data)
{
    uintptr_t *range = (uintptr_t *)data;
    int i;

    (void)size;
    range[0] = UINTPTR_MAX;
    range[1] = 0;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD || !(ph->p_flags & PF_X))
            continue;           /* code only, as on macOS */
        if (info->dlpi_addr + ph->p_vaddr < range[0])
            range[0] = info->dlpi_addr + ph->p_vaddr;
        if (info->dlpi_addr + ph->p_vaddr + ph->p_memsz > range[1])
            range[1] = info->dlpi_addr + ph->p_vaddr + ph->p_memsz;
    }
    return 1;                       /* the executable is listed first */
}

int host_module_range(uintptr_t *lo, uintptr_t *hi)
{
    uintptr_t range[2] = { 0, 0 };

    dl_iterate_phdr(first_object, range);
    if (range[1] <= range[0])
        return 0;
    *lo = range[0];
    *hi = range[1];
    return 1;
}
#endif

/* ---- the title's thread ---------------------------------------------- */

typedef struct {
    int argc;
    char **argv;
    int (*body)(int, char **);
    const host_loop *loop;
    int code;
} body_start;

static void *body_thread(void *arg)
{
    body_start *s = (body_start *)arg;

    recomp_fault_thread_init();
    s->code = s->body(s->argc, s->argv);
    if (s->loop && s->loop->run && s->loop->quit)
        s->loop->quit(s->code);
    return NULL;
}

/* The arguments the process started with, for host_relaunch_self. */
static char **g_start_argv;

int host_relaunch_self(void)
{
    char exe[PATH_MAX];
    char *fallback[2];

    if (!host_exe_path(exe, sizeof exe))
        return 0;
    fflush(stdout);
    fflush(stderr);
    if (g_start_argv && g_start_argv[0]) {
        execv(exe, g_start_argv);
    } else {
        fallback[0] = exe;
        fallback[1] = NULL;
        execv(exe, fallback);
    }
    return 0;                       /* execv returned: it failed */
}

int host_main_posix(int argc, char **argv, int (*body)(int argc, char **argv),
                    const host_loop *loop)
{
    static body_start s;
    pthread_attr_t attr;
    pthread_t thread;
    const char *mb = getenv("RECOMP_MAIN_STACK_MB");
    long stack_mb = mb ? strtol(mb, NULL, 0) : 0;

    /* Lifted code calls lifted code as C, so guest recursion is host
     * recursion; the guest's own stack lives in guest memory. 64 MB is
     * only reserved, not committed. */
    if (stack_mb <= 0)
        stack_mb = 64;
    g_start_argv = argv;
    s.argc = argc;
    s.argv = argv;
    s.body = body;
    s.loop = loop;
    s.code = 0;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)stack_mb << 20);
    if (pthread_create(&thread, &attr, body_thread, &s) != 0) {
        pthread_attr_destroy(&attr);
        fprintf(stderr, "[BOOT] cannot start the title thread (%ld MB stack); "
                        "running on the main thread\n", stack_mb);
        return body(argc, argv);
    }
    pthread_attr_destroy(&attr);

    if (loop && loop->run && loop->quit)
        return loop->run();         /* body_thread ends it with its code */
    pthread_join(thread, NULL);
    return s.code;
}

#endif /* !_WIN32 */
