"""
A biased frame pointer is a frame: `push ebp; lea ebp, [esp - N]`.

Run: py -3 tools/recomp/test_biased_frame.py

MSVC emits it to reach more locals with 8-bit displacements. Pro Evolution
Soccer 5's libjpeg decoder (sub_004F5929) opens `push ebp; lea ebp,
[esp-0x70]; sub esp, 0x274` and calls setjmp. The frame was never published,
so the guest setjmp saved the caller's frame, the longjmp back restored it,
and the function returned with esp 0x90 too high.
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.recomp import config  # noqa: E402
from tools.recomp.translator import FunctionTranslator  # noqa: E402

BASE = 0x00010000

# Every global config._install writes, so this test cannot leak into another.
_CONFIG_GLOBALS = (
    "_SECTIONS", "SECTIONS", "_configured_from",
    "TEXT_VA_START", "TEXT_VA_END", "RDATA_VA_START", "RDATA_VA_END",
    "DATA_VA_START", "DATA_VA_END", "KERNEL_THUNK_ADDR", "ENTRY_POINT",
)


def _translate(code, callee_at):
    saved = {k: getattr(config, k) for k in _CONFIG_GLOBALS}
    try:
        return _translate_installed(code, callee_at)
    finally:
        for k, v in saved.items():
            setattr(config, k, v)


def _translate_installed(code, callee_at):
    image = code + b"\xCC" * (callee_at - len(code)) + b"\xC3"
    config._install(
        [config.Section(".text", BASE, len(image), 0x0000, len(image), True)],
        entry_point=BASE, kernel_thunk_addr=BASE, origin="biased-frame-test")
    db = {BASE: {"start": f"0x{BASE:08X}", "end": BASE + len(code),
                 "_addr": BASE, "size": len(code)},
          BASE + callee_at: {"start": f"0x{BASE + callee_at:08X}",
                             "end": BASE + callee_at + 1,
                             "_addr": BASE + callee_at, "size": 1}}
    return FunctionTranslator(image, db).translate_function(BASE, db[BASE])


def test_a_biased_frame_is_published_and_kept_across_calls():
    """
    0x00  55                   push ebp
    0x01  8D 6C 24 90          lea ebp, [esp-0x70]
    0x05  81 EC 74 02 00 00    sub esp, 0x274
    0x0B  E8 <rel32>           call BASE+0x40
    0x10  83 C5 70             add ebp, 0x70
    0x13  C9                   leave
    0x14  C2 08 00             ret 8
    """
    code = bytes.fromhex("55" "8D6C2490" "81EC74020000")
    code += b"\xE8" + struct.pack("<i", 0x40 - 0x10)
    code += bytes.fromhex("83C570" "C9" "C20800")
    c = _translate(code, 0x40)
    lea = c.index("ebp = esp + -112;")
    after_lea = c[lea:]
    assert after_lea.index("g_seh_ebp = ebp;") < after_lea.index("sub_00010040"), c
    # ...and re-published before the call, as a `mov ebp, esp` frame is.
    assert "g_seh_ebp = ebp;\n    PUSH32(esp, 0x00010010u)" in c, c


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
