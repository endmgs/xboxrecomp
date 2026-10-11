"""
Self-check for long bodies in FunctionDetector._pass_data_ptr_targets.

Run: py -3 tools/disasm/test_data_ptr_long_body.py

A table entry in a gap is accepted outright when its body reaches a ret within
64 instructions. A longer one needs two more pieces of evidence: another entry
in the same table run is a known function, and the target starts after
padding or a ret/jmp. Burnout 2's vtable methods 0x000419C0 and 0x0004B7B0
need the first; the .XTLID ids that fall in .text's range and land
mid-function are what the second keeps out.
"""

import os
import sys
from types import SimpleNamespace

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.disasm.engine import DisasmEngine  # noqa: E402
from tools.disasm.functions import FunctionDetector  # noqa: E402
from tools.disasm.test_alias_tail_jumps import _Img  # noqa: E402
from tools.disasm.test_decode_at import _Section  # noqa: E402

TEXT = 0x00010000
DATA = 0x00020000
LONG = b"\x89\xc0" * 70 + b"\xc3"      # 70 x mov eax,eax; ret -- past 64

KNOWN = TEXT + 0x000       # a known function
AFTER_PAD = TEXT + 0x010   # long body, after int3 padding
FALL_IN = TEXT + 0x114     # long body reached by falling out of a mov
# Long body after padding with no known table mate. Not 16-aligned, so the
# padded-entry rule (which would accept it) stays out of the way and the
# table-mate rule is what decides.
LONELY = TEXT + 0x301


class _Img2(_Img):
    @property
    def sections(self):
        return list(self._secs)


def _detector(table_words):
    code = bytearray(b"\xcc" * 0x400)
    code[0x000] = 0xc3
    code[0x010:0x010 + len(LONG)] = LONG
    code[0x100:0x114] = b"\x89\xc0" * 10            # falls through into 0x114
    code[0x114:0x114 + len(LONG)] = LONG
    code[0x301:0x301 + len(LONG)] = LONG
    # A function ending in `ret 0Ch` exactly on a 16-byte boundary, and the
    # next one starting right there with no padding (Halo 2's tag accessors).
    code[0x200:0x20D] = b"\x89\xc0" * 6 + b"\x90"
    code[0x20D:0x210] = b"\xc2\x0c\x00"           # ret 0Ch, ends at 0x210
    code[0x210:0x218] = b"\x8b\x44\x24\x04\xc2\x0c\x00\x90"  # mov eax,[esp+4]; ret 0Ch
    # The same body after a mov that falls into it: not a boundary.
    code[0x220:0x22E] = b"\x89\xc0" * 7
    code[0x22E:0x230] = b"\x89\xc0"
    code[0x230:0x237] = b"\x8b\x44\x24\x04\xc2\x0c\x00"
    text = _Section(TEXT, bytes(code))
    text.data = bytes(code)

    data = b"".join(w.to_bytes(4, "little") for w in table_words)
    rdata = _Section(DATA, data, executable=False)
    rdata.data = data
    rdata.name = ".rdata"

    image = _Img2(text, rdata)
    engine = DisasmEngine(image)
    engine.linear_sweep(text)
    det = FunctionDetector.__new__(FunctionDetector)
    det.engine = engine
    det.image = image
    det._candidates = {}
    det._alias_entries = {}
    det.functions = {KNOWN: SimpleNamespace(start=KNOWN, end=KNOWN + 1)}
    return det, text


def test_a_long_vtable_method_beside_a_known_function_is_found():
    # The known function is two slots along, not adjacent -- as in Burnout 2.
    det, text = _detector([AFTER_PAD, TEXT + 0x20, KNOWN, 0])
    det._pass_data_ptr_targets([text])
    assert AFTER_PAD in det._alias_entries, det._alias_entries


def test_a_long_body_reached_by_fall_through_is_not():
    det, text = _detector([FALL_IN, KNOWN, 0])
    det._pass_data_ptr_targets([text])
    assert FALL_IN not in det._alias_entries, det._alias_entries


def test_a_long_body_with_no_known_table_mate_is_not():
    det, text = _detector([LONELY, LONELY + 4, 0])
    det._pass_data_ptr_targets([text])
    assert LONELY not in det._alias_entries, det._alias_entries


def test_a_lone_word_at_a_padded_function_is_found():
    # Burnout 2's 0x0010A9F0: a callback in a registration record, with no
    # neighbouring code pointer, 16-aligned straight after padding.
    det, text = _detector([AFTER_PAD, 0])
    det._pass_data_ptr_targets([text])
    assert AFTER_PAD in det._alias_entries, det._alias_entries


def test_a_lone_word_at_an_unaligned_target_is_not():
    # The same, one byte off the boundary: a lone word is a coincidence unless
    # it lands exactly where a compiler puts a function.
    det, text = _detector([LONELY, 0])
    det._pass_data_ptr_targets([text])
    assert LONELY not in det._alias_entries, det._alias_entries


RET_ADJ = TEXT + 0x210
FALL_ADJ = TEXT + 0x230


def test_a_lone_word_right_after_a_ret_is_found():
    # Halo 2's 0x002AC4B0: 16-aligned, straight after the previous function's
    # `ret 0Ch`, named by one word in a record of small numbers.
    det, text = _detector([4, RET_ADJ, 0])
    det._pass_data_ptr_targets([text])
    assert RET_ADJ in det._alias_entries, det._alias_entries


def test_a_lone_word_after_a_fall_through_is_not():
    # The same body, 16-aligned, but code runs into it: the middle of
    # something, whatever the word says.
    det, text = _detector([4, FALL_ADJ, 0])
    det._pass_data_ptr_targets([text])
    assert FALL_ADJ not in det._alias_entries, det._alias_entries


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
