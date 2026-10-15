"""Lifted arithmetic wraps, as x86 does, whatever the compiler assumes.

C's signed arithmetic is undefined on overflow and clang and GCC optimise on
the assumption that it does not happen. TimeSplitters 2's sub_00051C20 does

    cmp eax, -1 ; jg ...         (so eax >= 0 on the path below)
    imul eax, eax, 0x34
    mov edx, [eax + 0x2A0FD8]

and the lifter used to write the multiply as (int32_t)eax * (int32_t)0x34.
Knowing eax >= 0 and that the product "cannot" overflow, clang -O3 on arm64
built the address as one 64-bit multiply-add (umaddl), past XBOX_PTR's
(uint32_t) truncation: for eax = 0x7FFFFFFF the load went 104 GB beyond guest
memory, where x86 wraps to 0x002A0FA4.

The fix is in two places (CMakeLists.txt, "Lifted code wraps"): the lifter
writes imul's low half and neg as uint32_t arithmetic, which wraps on every
compiler including MSVC, and on clang/GCC lifted sources also build with
-fwrapv. These tests compile the emitted text WITHOUT -fwrapv, so they check
the first half alone.

As in test_lifter_sf_overflow, operands come through a volatile so the
constant folder cannot answer for the emitted code.
"""
import os
import shutil
import subprocess
import tempfile

import pytest

from .disasm import BasicBlock, Instruction, Operand
from .lifter import Lifter, lift_basic_block


def _reg(name):
    return Operand(type="reg", reg=name)


def _imm(value):
    return Operand(type="imm", imm=value)


def _mem(size, base="ecx"):
    return Operand(type="mem", mem_base=base, mem_index=None, mem_scale=1,
                   mem_disp=0, mem_size=size)


def _lift(mnemonic, *ops):
    insn = Instruction(0x10000, 3, mnemonic, "", "", operands=list(ops))
    lifted, _ = lift_basic_block(Lifter(), BasicBlock(start=0x10000, instructions=[insn]))
    return "\n".join(lifted)


def _cc():
    return os.environ.get("CC") or shutil.which("cc") or shutil.which("gcc")


# ── the text ──────────────────────────────────────────────────────────────

@pytest.mark.parametrize("ops", [
    (_reg("eax"), _reg("eax"), _imm(0x34)),
    (_reg("eax"), _reg("ecx"), _imm(-4)),
    (_reg("eax"), _reg("ecx")),
    (_reg("eax"), _mem(4)),
    (_reg("ax"), _reg("cx")),
    (_reg("ax"), _reg("cx"), _imm(0x7FFF)),
])
def test_imul_low_half_is_unsigned(ops):
    text = _lift("imul", *ops)
    assert "(int32_t)" not in text and "(int16_t)" not in text, text
    assert "(uint32_t)" in text


@pytest.mark.parametrize("op", [_reg("eax"), _reg("ax"), _reg("al"), _mem(4)])
def test_neg_is_unsigned(op):
    first = _lift("neg", op).splitlines()[0]
    assert "-(int" not in first, first
    assert "0u - (uint32_t)" in first, first


# ── compiled, optimised, no -fwrapv ───────────────────────────────────────

_PRELUDE = r"""
#include <stdint.h>
#include <stdio.h>
#define LO16(v) ((uint16_t)(v))
#define LO8(v) ((uint8_t)(v))
#define SET_LO16(r, v) ((r) = ((r) & 0xFFFF0000u) | (uint16_t)(v))
#define SET_LO8(r, v) ((r) = ((r) & 0xFFFFFF00u) | (uint8_t)(v))
/* XBOX_PTR as recomp_types.h has it. The offset is 0 here, but a global the
 * compiler cannot fold: added to the address, it is what let clang widen the
 * multiply into one 64-bit multiply-add. */
uintptr_t g_xbox_mem_offset;
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
static volatile uint32_t g_in[4];
static int bad;
#define WANT(got, want, what) do { uint64_t _g = (uint64_t)(got); \
    if (_g != (uint64_t)(want)) { \
    printf("FAIL %s: got 0x%llx want 0x%llx\n", what, \
           (unsigned long long)_g, (unsigned long long)(want)); bad++; } } while (0)
"""


def _compile_and_run(body, flags):
    cc = _cc()
    if not cc:
        pytest.skip("no C compiler available")
    src = _PRELUDE + body + "\nint main(void) { run(); puts(bad ? \"BAD\" : \"OK\"); return bad != 0; }\n"
    with tempfile.TemporaryDirectory() as d:
        c = os.path.join(d, "t.c")
        exe = os.path.join(d, "t")
        with open(c, "w") as f:
            f.write(src)
        built = subprocess.run([cc, *flags, "-o", exe, c], capture_output=True, text=True)
        if built.returncode != 0:
            return None, built.stderr
        r = subprocess.run([exe], capture_output=True, text=True)
        return r.returncode, r.stdout + r.stderr


def _ts2_shape():
    """sub_00051C20's path, as the lifter emits it: the address it loads from."""
    imul = _lift("imul", _reg("eax"), _reg("eax"), _imm(0x34))
    # The registers live in a global, as g_cpu's do.
    return r"""
struct { uint32_t eax; } g_regs;
#define eax g_regs.eax
__attribute__((noinline)) uintptr_t address(void) {
    if ((int32_t)eax > -1) {
        %s
        return XBOX_PTR(eax + 0x2A0FD8);
    }
    return 0;
}
static void run(void) {
    g_in[0] = 0x7FFFFFFFu; g_in[1] = 0x02000000u; g_in[2] = 3u;
    eax = g_in[0]; WANT(address(), 0x002A0FA4u, "0x7FFFFFFF * 0x34 + 0x2A0FD8 wraps");
    eax = g_in[1]; WANT(address(), 0x682A0FD8u, "0x02000000 * 0x34 + 0x2A0FD8 wraps");
    eax = g_in[2]; WANT(address(), 0x002A1074u, "3 * 0x34 + 0x2A0FD8");
}
#undef eax
""" % imul


def _arith():
    imul2 = _lift("imul", _reg("eax"), _reg("ecx"))
    imul16 = _lift("imul", _reg("ax"), _reg("cx"))
    neg = _lift("neg", _reg("eax")).splitlines()[0]
    return r"""
static void run(void) {
    uint32_t eax, ecx;
    g_in[0] = 0x80000000u; g_in[1] = 0xFFFFFFFFu; g_in[2] = 0x7FFFFFFFu; g_in[3] = 0x0000FFFFu;
    eax = g_in[0]; ecx = g_in[1]; %s
    WANT(eax, 0x80000000u, "imul INT_MIN, -1");
    eax = g_in[2]; ecx = g_in[2]; %s
    WANT(eax, 0x00000001u, "imul INT_MAX, INT_MAX");
    eax = g_in[3]; ecx = g_in[3]; %s
    WANT(eax, 0x00000001u, "imul ax, cx with 0xFFFF each");
    eax = g_in[0]; %s
    WANT(eax, 0x80000000u, "neg INT_MIN");
    eax = g_in[2]; %s
    WANT(eax, 0x80000001u, "neg INT_MAX");
}
""" % (imul2, imul2, imul16, neg, neg)


@pytest.mark.parametrize("opt", ["-O2", "-O3"])
def test_ts2_address_wraps_at_O(opt):
    rc, out = _compile_and_run(_ts2_shape(), [opt, "-fno-wrapv"])
    assert rc is not None, out
    assert rc == 0, "the lifted address does not wrap as x86's does:\n" + out


@pytest.mark.parametrize("opt", ["-O2", "-O3"])
def test_arithmetic_wraps_at_O(opt):
    rc, out = _compile_and_run(_arith(), [opt, "-fno-wrapv"])
    assert rc is not None, out
    assert rc == 0, out


def test_emitted_arithmetic_is_defined():
    """UBSan finds no signed overflow in the emitted text, on the inputs that
    overflow a signed multiply or negate: the property itself, whatever the
    optimiser does with it this year."""
    for body in (_ts2_shape(), _arith()):
        rc, out = _compile_and_run(body, ["-O1", "-fsanitize=signed-integer-overflow",
                                          "-fno-sanitize-recover=all"])
        if rc is None:
            pytest.skip("this compiler has no signed-integer-overflow sanitiser")
        assert "runtime error" not in out, out
        assert rc == 0, out
