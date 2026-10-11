/*
 * host_main.h - what a title's main.c needs from the host operating system.
 *
 * A title's start-up (templates/new-game/src/main.c) is portable C: find the
 * game, load the XBE, bring up the runtime, call the entry point. The pieces
 * of it that are not -- where the executable is, where its output goes, how
 * a fatal message is shown, how a host address is named, and on POSIX which
 * thread the title runs on -- are here, with a Windows half
 * (host_main_win32.c) and a POSIX half (host_main_posix.c).
 */
#ifndef HOST_MAIN_H
#define HOST_MAIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The running executable's full path. 0 when it cannot be found. */
int host_exe_path(char *buf, size_t bytes);

/* The directory part of a path, in place (the last separator and what
 * follows it removed). Either separator counts, on every host. */
void host_path_dirname(char *path);

/* 1 when `path` names a file (not a directory). */
int host_file_exists(const char *path);

/* macOS: when the executable is the one inside an application bundle
 * (<name>.app/Contents/MacOS/<exe>), the bundle's own path ("<...>/<name>.app")
 * in `buf`, and 1. 0 for a plain executable, and always 0 off Apple. A
 * bundle looks for its game beside itself and in Contents/Resources, and logs
 * to ~/Library/Logs/xboxrecomp rather than into itself. */
int host_app_bundle_dir(char *buf, size_t bytes);

/* Where the diagnostics go. A program started by double-click (Windows) or
 * from the Finder (macOS) has nowhere to print, so:
 *   1. output already redirected (a script capturing it, a pipe): left alone;
 *   2. started from a terminal: written there;
 *   3. otherwise: <executable>.log beside the program, truncated each run
 *      (for an executable inside a macOS .app: ~/Library/Logs/xboxrecomp/
 *      <executable>.log, so a run never writes into its signed bundle).
 * In case 3 the log's path is written to `log_path` (else it is set to ""),
 * so a failure can name it: "it did not start" is not a bug report, and the
 * file is. */
void host_setup_output(char *log_path, size_t bytes);

/* A fatal message the player has to see. Windows: a message box. POSIX:
 * nothing more than the caller's own stderr line -- a dialog would take the
 * focus, and the log already holds it. */
void host_error_box(const char *title, const char *message);

/* The symbol a host address is in, and the offset into it: the generated
 * sub_XXXXXXXX names a host pc back to the guest function. 0 when unknown.
 * Windows: dbghelp (symbols loaded by recomp_fault_install, PDB beside the
 * executable). POSIX: dladdr, which sees the executable's exported symbols
 * (Mach-O exports them by default; Linux titles link with -rdynamic). */
int host_symbol_name(uintptr_t addr, char *name, size_t bytes, uintptr_t *offset);

/* The address range of the executable's own image (on POSIX its code
 * alone), to tell its return addresses from other words on a stack. 0 when
 * unknown. */
int host_module_range(uintptr_t *lo, uintptr_t *hi);

/* Start this executable again, with the same arguments and the current
 * environment, and end this process: what a console's quick reboot into the
 * same title amounts to. POSIX: execv, so the process id (and anything
 * waiting on it, a timeout included) carries over. Windows: CreateProcess
 * with the standard handles inherited, then ExitProcess. Returns only when
 * the new image could not be started. */
int host_relaunch_self(void);

/* ---- POSIX: which thread the title runs on --------------------------- */

/* A host's main-thread event loop (src/host's recomp_host_loop_run/_quit).
 * On macOS every window belongs to the thread that ran main(), so that
 * thread must be free to serve one. */
typedef struct host_loop {
    int  (*run)(void);       /* until quit; returns quit's code */
    void (*quit)(int code);  /* from any thread */
} host_loop;

/* Run `body` -- the title's start-up and the game -- on a thread of its own
 * with a large stack (64 MB, or RECOMP_MAIN_STACK_MB), while this, the
 * process's main thread, runs `loop`. When body returns its code is handed
 * to loop->quit, and this returns what loop->run does. With no loop (NULL,
 * or run NULL) this just waits for body. Call it from main() and return
 * its result. POSIX only: on Windows main.c calls body directly. */
int host_main_posix(int argc, char **argv, int (*body)(int argc, char **argv),
                    const host_loop *loop);

#ifdef __cplusplus
}
#endif

#endif /* HOST_MAIN_H */
