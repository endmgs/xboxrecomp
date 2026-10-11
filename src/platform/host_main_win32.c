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

void host_setup_output(char *log_path, size_t bytes)
{
    char exe[MAX_PATH];
    char *dot;
    FILE *f;

    log_path[0] = '\0';
    if (handle_is_real(STD_OUTPUT_HANDLE) || handle_is_real(STD_ERROR_HANDLE))
        return;                                  /* redirected: leave it */

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

int host_relaunch_self(void)
{
    char exe[MAX_PATH];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    if (!host_exe_path(exe, sizeof exe))
        return 0;
    fflush(stdout);
    fflush(stderr);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    /* NULL environment: the child takes this process's current block, which
     * is where the caller put the launch data. */
    if (!CreateProcessA(exe, GetCommandLineA(), NULL, NULL, TRUE, 0, NULL,
                        NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

#endif /* _WIN32 */
