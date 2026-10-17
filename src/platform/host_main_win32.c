/*
 * host_main_win32.c - host_main.h on Windows. The code is what every title's
 * main.c carried, moved here unchanged in behaviour.
 */
#ifdef _WIN32

#include "host_main.h"

#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <io.h>        /* _open_osfhandle, _dup2 */

int host_exe_path(char *buf, size_t bytes)
{
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)bytes);
    return n != 0 && n < (DWORD)bytes;
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
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL handle_is_real(DWORD which)
{
    HANDLE h = GetStdHandle(which);

    if (h == NULL || h == INVALID_HANDLE_VALUE)
        return FALSE;
    return GetFileType(h) != FILE_TYPE_UNKNOWN;
}

/* A log on disk, written from a thread of its own.
 *
 * The runtime prints a great deal and flushes after nearly every line, and
 * stdout/stderr are unbuffered on top. Straight to a file that is a disk
 * write on whichever thread printed -- usually the game's -- and a write
 * that stalls (the antivirus scanning the growing log, the disk busy) stops
 * the game with it: Max Payne spent 40% of its main thread in WriteFile and
 * dropped frames in 45-150 ms lumps. So stdout and stderr go into a pipe,
 * and this thread moves the pipe to the file. A print is then a copy into
 * the pipe's buffer.
 * ponytail: a hard crash can lose what is still in the pipe (up to its 1 MB
 * buffer, normally a few lines); the fault handler's report is printed
 * before the process goes and the drain keeps up with that. */
static HANDLE g_log_pipe_read, g_log_target;

static DWORD WINAPI log_drain(LPVOID unused)
{
    static char buf[65536];
    DWORD got, put;

    (void)unused;
    while (ReadFile(g_log_pipe_read, buf, sizeof buf, &got, NULL) && got)
        WriteFile(g_log_target, buf, got, &put, NULL);
    return 0;
}

static int log_through_pipe(HANDLE target)
{
    HANDLE w;
    int fd;

    /* A copy: the freopen below closes the descriptor that may own target. */
    if (!DuplicateHandle(GetCurrentProcess(), target, GetCurrentProcess(), &g_log_target,
                         0, FALSE, DUPLICATE_SAME_ACCESS))
        return 0;
    if (!CreatePipe(&g_log_pipe_read, &w, NULL, 1u << 20)) {
        CloseHandle(g_log_target);
        return 0;
    }
    /* A windowed program's stdout/stderr may have no descriptor to dup
     * onto; NUL gives them one first. */
    freopen("NUL", "w", stdout);
    freopen("NUL", "w", stderr);
    fd = _open_osfhandle((intptr_t)w, 0);
    if (fd < 0 || _dup2(fd, _fileno(stdout)) != 0 || _dup2(fd, _fileno(stderr)) != 0)
        return 0;
    SetStdHandle(STD_OUTPUT_HANDLE, w);
    SetStdHandle(STD_ERROR_HANDLE, w);
    CloseHandle(CreateThread(NULL, 0, log_drain, NULL, 0, NULL));
    return 1;
}

void host_setup_output(char *log_path, size_t bytes)
{
    char exe[MAX_PATH];
    char *dot;
    FILE *f;

    log_path[0] = '\0';
    if (handle_is_real(STD_OUTPUT_HANDLE) || handle_is_real(STD_ERROR_HANDLE)) {
        /* Redirected: leave it where it was sent, but a file there gets
         * the same drain thread a log of our own does. */
        HANDLE e = GetStdHandle(STD_ERROR_HANDLE);
        if (e && e != INVALID_HANDLE_VALUE && GetFileType(e) == FILE_TYPE_DISK)
            log_through_pipe(e);
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {  /* started from a terminal */
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return;
    }

    if (!GetModuleFileNameA(NULL, exe, (DWORD)sizeof exe))
        return;
    snprintf(log_path, bytes, "%s", exe);
    dot = strrchr(log_path, '.');
    if (dot && !strchr(dot, '\\'))
        *dot = '\0';
    strncat(log_path, ".log", bytes - strlen(log_path) - 1);

    {
        HANDLE h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        int piped = h != INVALID_HANDLE_VALUE && log_through_pipe(h);
        if (h != INVALID_HANDLE_VALUE)
            CloseHandle(h);                      /* the drain holds its own copy */
        if (piped)
            return;
    }
    f = freopen(log_path, "w", stderr);
    if (!f) {                                    /* read-only folder */
        log_path[0] = '\0';
        return;
    }
    freopen(log_path, "a", stdout);
}

void host_error_box(const char *title, const char *message)
{
    MessageBoxA(NULL, message, title, MB_ICONERROR);
}

int host_symbol_name(uintptr_t addr, char *name, size_t bytes, uintptr_t *offset)
{
    /* SYMBOL_INFO is variable-length: the name is written past the struct, so
     * it must be over-allocated with MaxNameLen set to the slack. */
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;

    memset(buf, 0, sizeof(buf));
    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
    sym->MaxNameLen = 255;
    if (!SymFromAddr(GetCurrentProcess(), (DWORD64)addr, &disp, sym))
        return 0;
    snprintf(name, bytes, "%s", sym->Name);
    if (offset)
        *offset = (uintptr_t)disp;
    return 1;
}

int host_module_range(uintptr_t *lo, uintptr_t *hi)
{
    /* This module's own range. Not the preferred base, which ASLR moves. */
    HMODULE mod = GetModuleHandleW(NULL);
    const IMAGE_NT_HEADERS *nt;

    if (!mod)
        return 0;
    nt = (const IMAGE_NT_HEADERS *)
        ((const BYTE *)mod + ((const IMAGE_DOS_HEADER *)mod)->e_lfanew);
    *lo = (uintptr_t)mod;
    *hi = *lo + nt->OptionalHeader.SizeOfImage;
    return 1;
}

#endif /* _WIN32 */
