/*
 * recomp_cpu.h - the guest register file, one thread-local struct.
 *
 * The one declaration of the registers lifted code and the runtime share.
 * Generated code gets it through recomp_types.h (tools/recomp copies this
 * file into gen/ beside it on every lift); the runtime through
 * src/kernel/xbox_memory_layout.h, which includes this file from
 * templates/runtime by relative path; titles' main.c and recomp_manual.c
 * through those. Do not redeclare a register anywhere else: the names below
 * are macros, so `extern uint32_t g_eax;` no longer compiles, which is the
 * point -- a redeclaration with the wrong storage class used to link and
 * silently read other storage (recomp_manual.c says how that went).
 *
 * Why a struct. Every register used to be its own thread-local variable,
 * and on Mach-O each thread-local is reached through its own descriptor and
 * a call to _tlv_get_addr, so a lifted function touching eax, ecx, esp, ebx,
 * esi, edi and the x87 stack paid one call per register -- 62,023 such calls
 * across TimeSplitters 2's 6,883 lifted functions, about nine per function,
 * and a two-instruction getter became 21 instructions with two calls. One
 * struct is one descriptor: one call per function, after which every
 * register is an offset from the same base. ELF executables (initial-exec)
 * and Windows (one gs: load per function) were already cheap; the layout
 * changes nothing there.
 *
 * Generated code is unchanged: it still says eax, g_esp, g_fp_top, and the
 * macros at the bottom turn those into fields. The fields carry an r_ prefix
 * so no field name is also a macro (generated code defines eax as g_eax, and
 * g_eax expanding to a field called eax would re-expand inside itself).
 *
 * The SSE/MMX registers stay separate thread-locals in recomp_types_simd.h:
 * few lifted files touch them, and those that do not are compiled without
 * that header at all.
 *
 * Changing the layout: generated code is compiled against the copy of this
 * file in its gen/ folder and the runtime against this one, so a layout
 * change must also rename the variable (g_cpu), or a title lifted before the
 * change would link and read the wrong fields. Adding a register at the end
 * is still a layout change for that purpose.
 */
#ifndef RECOMP_CPU_H
#define RECOMP_CPU_H

#include <stdint.h>

/* Thread-local storage class for the register set. */
#ifndef RECOMP_TLS
#  if defined(_MSC_VER)
#    define RECOMP_TLS __declspec(thread)
#  elif defined(__GNUC__) || defined(__clang__)
#    define RECOMP_TLS __thread
#  else
#    define RECOMP_TLS _Thread_local
#  endif
#endif

struct recomp_cpu {
    /* Integer registers. ebp is a local in each lifted function; r_ebp is
     * "the last frame established anywhere" (see g_ebp in recomp_types.h)
     * and r_seh_ebp carries ebp across the SEH helpers. */
    uint32_t r_eax, r_ecx, r_edx, r_esp;
    uint32_t r_ebx, r_esi, r_edi;
    uint32_t r_ebp, r_seh_ebp;
    /* Linear base of fs: where this thread's TIB lives (XBOX_FS_BASE). */
    uint32_t r_fs_base;
    /* EFLAGS.DF. */
    int      r_df;
    /* x87: the stack, its top, control word, the last compare and the
     * condition codes FNSTSW reads. */
    int      r_fp_top;
    double   r_fp_stack[8];
    uint16_t r_fp_control_word;
    uint16_t r_fp_cc;
    int      r_fp_cmp;
    /* The esp and the form (1 call, 2 jump) of the last refused indirect
     * dispatch, for recomp_icall_fail_log. */
    uint32_t r_icall_saved_esp;
    uint32_t r_icall_dispatch_form;
};

extern RECOMP_TLS struct recomp_cpu g_cpu;

/* The caller's own frame while an LTCG replacement runs.
 *
 * An LTCG build passes some arguments in registers, so the generated thunk
 * (RECOMP_HLE_LTCG_CALL in recomp_types.h) gives the replacement an ordinary
 * argument frame of its own. A replacement that also runs the title's body
 * (HLE_CALL_ORIGINAL) must hand that body the caller's real frame and
 * registers instead, or the body reads its stack arguments from the
 * synthetic frame -- Halo 2's Direct3D_CreateDevice read its BehaviorFlags
 * as the presentation parameters pointer and failed. The thunk records them
 * here for the length of the call: esp as the thunk found it, the argument
 * count of the synthetic frame (0 outside an LTCG thunk), and eax, ecx, edx,
 * ebx, esi, edi. Separate from g_cpu so that a title lifted before this
 * existed keeps the same register layout. */
extern RECOMP_TLS uint32_t g_hle_ltcg_esp;
extern RECOMP_TLS uint32_t g_hle_ltcg_nargs;
extern RECOMP_TLS uint32_t g_hle_ltcg_regs[6];

#define g_eax                   (g_cpu.r_eax)
#define g_ecx                   (g_cpu.r_ecx)
#define g_edx                   (g_cpu.r_edx)
#define g_esp                   (g_cpu.r_esp)
#define g_ebx                   (g_cpu.r_ebx)
#define g_esi                   (g_cpu.r_esi)
#define g_edi                   (g_cpu.r_edi)
#define g_ebp                   (g_cpu.r_ebp)
#define g_seh_ebp               (g_cpu.r_seh_ebp)
#define g_fs_base               (g_cpu.r_fs_base)
#define g_df                    (g_cpu.r_df)
#define g_fp_top                (g_cpu.r_fp_top)
#define g_fp_stack              (g_cpu.r_fp_stack)
#define g_fp_control_word       (g_cpu.r_fp_control_word)
#define g_fp_cc                 (g_cpu.r_fp_cc)
#define g_fp_cmp                (g_cpu.r_fp_cmp)
#define g_icall_saved_esp       (g_cpu.r_icall_saved_esp)
#define g_icall_dispatch_form   (g_cpu.r_icall_dispatch_form)

#endif /* RECOMP_CPU_H */
