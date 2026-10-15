"""
Patch an XBE before it is disassembled and lifted.

Lifted code bakes every address and immediate in as a constant, so a title
whose fixed-size table has to grow cannot be fixed at run time: the code that
indexes the table at its old address, and compares against its old count, is
already C. The fix has to go into the image the pipeline reads. This module
takes a patch file (JSON, see docs/technical/xbe-patches.md) and the title's
own default.xbe, and returns the patched image the stages work on. The
player's default.xbe is never written.

The runtime loads the unpatched default.xbe from the game folder, so the same
changes have to reach it too. render_c() turns the difference between the two
images into a C file the recomp stage links into the title (a list of byte
runs, checked against the SHA-1 of both images); src/kernel/xbe_patches.c
applies it to the file in memory before the loader maps a single section. One
implementation of the operations (this one), so what the title runs is the
image that was lifted, header included.

Operations, applied in order, each checked:

  add_section  a new section after the last one, page-aligned; the image grows
  patch        same-length bytes at a VA, refused unless the old bytes match,
               and in code, refused if any instruction changes length or kind
  copy         bytes from one VA to another (e.g. the old table into the new
               section), as the image is at load
  fill, write  bytes into a section this file added

copy, fill and write may only write into a section the patch file added:
every change to the title's own sections goes through `patch`, which states
the bytes it replaces.
"""

import hashlib
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

FORMAT = 1

PAGE = 0x1000
SECTHDR_SIZE = 0x38

SECTION_WRITABLE = 0x00000001
SECTION_PRELOAD = 0x00000002
SECTION_EXECUTABLE = 0x00000004

# Header fields (offsets from the start of the file, which maps at the base).
H_BASE = 0x104
H_SIZE_OF_HEADERS = 0x108
H_SIZE_OF_IMAGE = 0x10C
H_SIZE_OF_IMAGE_HEADER = 0x110
H_CERT = 0x118
H_SECTION_COUNT = 0x11C
H_SECTION_HEADERS = 0x120
H_TLS = 0x12C
H_PE_HEADER = 0x13C          # where the header keeps a copy of the PE headers
H_DEBUG_PATH = 0x14C
H_DEBUG_FILE = 0x150
H_DEBUG_UFILE = 0x154
H_KERNEL_THUNK = 0x158
H_LIB_COUNT = 0x160
H_LIB_VERSIONS = 0x164
H_LOGO = 0x170
H_LOGO_SIZE = 0x174

THUNK_XOR = (0x5B6D40B6, 0xEFB1F152)    # retail, debug

# The runtime maps the guest's RAM at 64 MB; nothing of the image may reach
# past it (xbox_memory_layout.c skips such a section).
RAM_END = 0x04000000

# Sections holding data even when flagged executable (tools.disasm.loader's
# list; a title adds its own with --data-sections). A patch in one of them is
# not decoded as instructions.
DATA_SECTION_NAMES = frozenset({
    ".data", ".data1", ".rdata", ".idata", ".edata", ".reloc", ".tls",
})

BLOB_MAGIC = b"XBEP"
BLOB_VERSION = 1


class PatchError(Exception):
    """A patch file that cannot be applied to this XBE, and why."""


@dataclass
class Section:
    index: int
    name: str
    flags: int
    va: int
    vsize: int
    raw: int
    rsize: int

    @property
    def end(self):
        return self.va + self.vsize


def _align(value, to):
    return (value + to - 1) & ~(to - 1)


class Image:
    """The XBE being patched: its bytes and what its header says."""

    def __init__(self, data: bytes):
        if len(data) < 0x178 or data[:4] != b"XBEH":
            raise PatchError("not an XBE (no XBEH magic)")
        self.data = bytearray(data)
        self.added = []          # names of the sections this patch file added

    def u32(self, off):
        return struct.unpack_from("<I", self.data, off)[0]

    def put32(self, off, value):
        struct.pack_into("<I", self.data, off, value & 0xFFFFFFFF)

    @property
    def base(self):
        return self.u32(H_BASE)

    def sections(self):
        count = self.u32(H_SECTION_COUNT)
        table = self.u32(H_SECTION_HEADERS) - self.base
        out = []
        for i in range(count):
            off = table + i * SECTHDR_SIZE
            if off + SECTHDR_SIZE > len(self.data):
                raise PatchError(f"section header {i} lies outside the file")
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from(
                "<6I", self.data, off)
            out.append(Section(i, self._cstr(name_va - self.base), flags, va,
                               vsize, raw, rsize))
        return out

    def _cstr(self, off):
        if not 0 <= off < len(self.data):
            return "?"
        end = self.data.find(b"\0", off)
        return self.data[off:end if end >= 0 else off].decode("latin-1")

    def section_named(self, name):
        for s in self.sections():
            if s.name == name:
                return s
        return None

    def section_at(self, va):
        for s in self.sections():
            if s.va <= va < s.end:
                return s
        return None

    # -- reading and writing by VA -------------------------------------------

    def _span(self, va, size, what):
        """(section, file offset) for [va, va+size): inside one section and
        backed by the file, or inside the headers."""
        base = self.base
        hdr = self.u32(H_SIZE_OF_HEADERS)
        if base <= va and va + size <= base + hdr:
            return None, va - base
        s = self.section_at(va)
        if s is None or va + size > s.end:
            raise PatchError(f"{what}: 0x{va:08X}+0x{size:X} is not inside one "
                             f"section of the image")
        if va - s.va + size > s.rsize:
            raise PatchError(f"{what}: 0x{va:08X}+0x{size:X} reaches past the "
                             f"bytes {s.name} has in the file (raw size "
                             f"0x{s.rsize:X}); the loader zeroes that part, so "
                             f"a change there would not survive loading")
        return s, s.raw + (va - s.va)

    def read(self, va, size, what):
        """Bytes at va as the loader leaves them: a section's part past its
        raw size reads as zero."""
        base = self.base
        hdr = self.u32(H_SIZE_OF_HEADERS)
        if base <= va and va + size <= base + hdr:
            return bytes(self.data[va - base:va - base + size])
        s = self.section_at(va)
        if s is None or va + size > s.end:
            raise PatchError(f"{what}: 0x{va:08X}+0x{size:X} is not inside one "
                             f"section of the image")
        out = bytearray(size)
        have = max(0, min(size, s.rsize - (va - s.va)))
        if have:
            off = s.raw + (va - s.va)
            out[:have] = self.data[off:off + have]
        return bytes(out)

    def write(self, va, payload, what):
        _s, off = self._span(va, len(payload), what)
        self.data[off:off + len(payload)] = payload

    # -- the header's free space --------------------------------------------

    def header_fixed(self):
        """[(start, end)] file offsets of everything in the headers that stays
        where it is when a section is added.

        The image header, certificate, section table, library versions, debug
        names, logo, the copy of the PE headers, and the kernel thunk table and
        TLS directory when an image keeps them there. Not the section names or
        the shared-page counters: those move (header_movable). A pointer outside
        the headers is ignored. Whatever this misses is still protected: new
        bytes only ever go where the file holds zeros.
        """
        base = self.base
        hdr = self.u32(H_SIZE_OF_HEADERS)
        out = [(0, self.u32(H_SIZE_OF_IMAGE_HEADER))]

        def inside(va):
            return base <= va < base + hdr

        def span(va, size):
            if inside(va) and size > 0:
                out.append((va - base, va - base + size))

        def cstr(va):
            if inside(va):
                span(va, len(self._cstr(va - base)) + 1)

        cert = self.u32(H_CERT)
        if inside(cert):
            span(cert, max(self.u32(cert - base), 0x1D0))
        span(self.u32(H_SECTION_HEADERS), self.u32(H_SECTION_COUNT) * SECTHDR_SIZE)
        span(self.u32(H_LIB_VERSIONS), self.u32(H_LIB_COUNT) * 16)
        cstr(self.u32(H_DEBUG_PATH))
        cstr(self.u32(H_DEBUG_FILE))
        uptr = self.u32(H_DEBUG_UFILE)
        if inside(uptr):
            o = uptr - base
            while o + 1 < len(self.data) and self.data[o:o + 2] != b"\0\0":
                o += 2
            out.append((uptr - base, o + 2))
        span(self.u32(H_LOGO), self.u32(H_LOGO_SIZE))
        span(self.u32(H_TLS), 0x18)
        pe = self.u32(H_PE_HEADER)
        if inside(pe) and self.data[pe - base:pe - base + 2] == b"MZ":
            o = pe - base
            lfanew = self.u32(o + 0x3C)
            coff = o + lfanew + 4
            if coff + 20 <= len(self.data):
                nsec, = struct.unpack_from("<H", self.data, coff + 2)
                opt, = struct.unpack_from("<H", self.data, coff + 16)
                out.append((o, coff + 20 + opt + nsec * 40))
        raw = self.u32(H_KERNEL_THUNK)
        for key in THUNK_XOR:
            thunk = raw ^ key
            if inside(thunk):
                o = thunk - base
                while o + 4 <= len(self.data) and self.u32(o) != 0:
                    o += 4
                out.append((thunk - base, o + 4))
                break
        return out

    def header_movable(self):
        """Each section's name and its head and tail shared-page counters, as
        ({section index: name}, {old counter offset: value}): what add_section
        lays out again. Counters are 16-bit and shared by neighbours (one
        section's tail is the next one's head)."""
        base = self.base
        hdr = self.u32(H_SIZE_OF_HEADERS)
        table = self.u32(H_SECTION_HEADERS) - base
        names, counters = {}, {}
        for i in range(self.u32(H_SECTION_COUNT)):
            off = table + i * SECTHDR_SIZE
            names[i] = self._cstr(self.u32(off + 0x14) - base)
            for field in (0x1C, 0x20):
                ptr = self.u32(off + field)
                if base <= ptr < base + hdr:
                    counters[ptr - base] = struct.unpack_from(
                        "<H", self.data, ptr - base)[0]
        return names, counters

    def header_free(self, size, align, avoid, start=0):
        """The first file offset at or after `start` where `size` bytes are
        zero, inside the headers' limit and clear of every span in `avoid`."""
        limit = self.header_limit()
        o = _align(max(start, self.u32(H_SIZE_OF_IMAGE_HEADER)), align)
        while o + size <= limit:
            clash = next((e for s, e in avoid if s < o + size and o < e), None)
            if clash is not None:
                o = _align(clash, align)
                continue
            nz = next((k for k in range(o + size - 1, o - 1, -1)
                       if self.data[k]), None)
            if nz is None:
                return o
            o = _align(nz + 1, align)
        return None

    def header_limit(self):
        """How far the headers may grow: to the first section, in memory and
        in the file, and never past the 64 KB the runtime copies."""
        base = self.base
        limit = min(0x10000, len(self.data))
        for s in self.sections():
            limit = min(limit, s.va - base)
            if s.rsize:
                limit = min(limit, s.raw)
        return limit


# ── the patch file ──────────────────────────────────────────────────────────

_ADDR_RE = re.compile(r"^\s*([A-Za-z_.$][\w.$]*)\s*(?:\+\s*(\S+))?\s*$")


def _int(value, what):
    if isinstance(value, bool):
        raise PatchError(f"{what}: expected a number, got {value!r}")
    if isinstance(value, int):
        return value
    if isinstance(value, str):
        try:
            return int(value, 0)
        except ValueError:
            pass
    raise PatchError(f"{what}: expected a number (e.g. 4096 or \"0x1000\"), "
                     f"got {value!r}")


def _addr(img, value, what):
    """A VA: a number, or SECTION / SECTION+offset (any section, including
    one this file added)."""
    if isinstance(value, str):
        m = _ADDR_RE.match(value)
        if m and not value.strip().lower().startswith("0x") \
                and not value.strip()[0].isdigit():
            s = img.section_named(m.group(1))
            if s is None:
                raise PatchError(f"{what}: no section named {m.group(1)!r}")
            return s.va + (_int(m.group(2), what) if m.group(2) else 0)
    return _int(value, what)


def _hex(value, what):
    if not isinstance(value, str):
        raise PatchError(f"{what}: expected a hex string, got {value!r}")
    text = re.sub(r"\s+", "", value)
    try:
        return bytes.fromhex(text)
    except ValueError:
        raise PatchError(f"{what}: {value!r} is not hex bytes") from None


_KEYS = {
    "add_section": ({"name", "size"}, {"va", "writable", "fill", "bytes"}),
    "patch": ({"va", "old", "new"}, {"insn", "data"}),
    "copy": ({"from", "to", "size"}, set()),
    "fill": ({"va", "size"}, {"value"}),
    "write": ({"va", "bytes"}, set()),
}
_NOTE_KEYS = {"op", "why", "comment"}


def load_spec(path):
    """Read and shape-check a patch file; returns the parsed dict."""
    try:
        spec = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise PatchError(f"{path}: {e}") from None
    if not isinstance(spec, dict) or not isinstance(spec.get("operations"), list):
        raise PatchError(f"{path}: expected an object with an \"operations\" list")
    if spec.get("format", FORMAT) != FORMAT:
        raise PatchError(f"{path}: format {spec.get('format')!r}; this toolkit "
                         f"reads format {FORMAT}")
    unknown = set(spec) - {"format", "xbe_sha1", "operations", "comment", "why"}
    if unknown:
        raise PatchError(f"{path}: unknown key(s) {sorted(unknown)}")
    for i, op in enumerate(spec["operations"]):
        where = f"{path}: operation {i}"
        if not isinstance(op, dict) or op.get("op") not in _KEYS:
            raise PatchError(f"{where}: \"op\" must be one of {sorted(_KEYS)}")
        need, opt = _KEYS[op["op"]]
        missing = need - set(op)
        extra = set(op) - need - opt - _NOTE_KEYS
        if missing:
            raise PatchError(f"{where} ({op['op']}): missing {sorted(missing)}")
        if extra:
            raise PatchError(f"{where} ({op['op']}): unknown key(s) {sorted(extra)}")
    return spec


# ── the operations ──────────────────────────────────────────────────────────

def _add_section(img, op, what, log):
    name = op["name"]
    if not isinstance(name, str) or not name or len(name) > 32 \
            or not all(0x21 <= ord(c) < 0x7F for c in name):
        raise PatchError(f"{what}: section name {name!r} must be 1-32 printable "
                         f"ASCII characters")
    sections = img.sections()
    if any(s.name == name for s in sections):
        raise PatchError(f"{what}: the image already has a section {name!r}")
    size = _int(op["size"], what + " size")
    if size <= 0:
        raise PatchError(f"{what}: size must be positive")
    fill = _int(op.get("fill", 0), what + " fill")
    if not 0 <= fill <= 0xFF:
        raise PatchError(f"{what}: fill is one byte (0-255)")
    initial = _hex(op["bytes"], what + " bytes") if "bytes" in op else b""
    if len(initial) > size:
        raise PatchError(f"{what}: {len(initial)} initial bytes for a "
                         f"0x{size:X}-byte section")

    base = img.base
    va = _align(max(s.end for s in sections), PAGE)
    if "va" in op and _int(op["va"], what + " va") != va:
        raise PatchError(f"{what}: {name} would be placed at 0x{va:08X}, not "
                         f"{op['va']} as the patch file expects (another XBE, "
                         f"or an earlier section changed size)")
    if va + size > RAM_END:
        raise PatchError(f"{what}: {name} would end at 0x{va + size:08X}, past "
                         f"the 64 MB the runtime maps")

    # The section table gains an entry, and every entry points at its name
    # and at two 16-bit shared-page counters, all in the headers. A retail
    # header has little room: TimeSplitters 2's is full to 0x1000 with the
    # names and counters packed right behind the table, then library
    # versions, debug names, the logo and a copy of the PE headers. So the
    # names and counters are laid out again in the free space, which lets the
    # table grow where it is; only if that space is taken by something that
    # cannot move does the whole table move. New bytes go only where the file
    # holds zeros, clear of everything the header points at.
    count = len(sections)
    hdr = img.u32(H_SIZE_OF_HEADERS)
    table = img.u32(H_SECTION_HEADERS) - base
    table_bytes = (count + 1) * SECTHDR_SIZE
    fixed = img.header_fixed()
    names, counters = img.header_movable()

    def clashes(start, end):
        return any(s < end and start < e for s, e in fixed)

    moved = {}
    for i in range(count):
        off = table + i * SECTHDR_SIZE
        p = img.u32(off + 0x14) - base
        n = len(names[i]) + 1
        if 0 <= p and p + n <= hdr and not clashes(p, p + n):
            moved[i] = names[i]
            img.data[p:p + n] = bytes(n)
    for c in counters:
        if clashes(c, c + 2):
            raise PatchError(f"{what}: a shared-page counter at header offset "
                             f"0x{c:X} overlaps other header data")
        img.data[c:c + 2] = b"\0\0"

    limit = img.header_limit()
    grow = table + count * SECTHDR_SIZE
    if (table + table_bytes <= limit and not clashes(grow, table + table_bytes)
            and not any(img.data[grow:table + table_bytes])):
        table_new = table
    else:
        table_new = img.header_free(table_bytes, 4, fixed)
        if table_new is None:
            raise PatchError(f"{what}: no room in the headers for a section "
                             f"table of {count + 1} entries (0x{table_bytes:X} "
                             f"bytes free and zero before 0x{limit:X})")
    avoid = fixed + [(table_new, table_new + table_bytes)]

    block = bytearray()
    counter_at = {}
    for c in sorted(counters):
        counter_at[c] = len(block)
        block += struct.pack("<H", counters[c])
    new_counters = len(block)
    block += bytes(4)
    name_at = {}
    for i, text in moved.items():
        name_at[i] = len(block)
        block += text.encode("latin-1") + b"\0"
    name_at[count] = len(block)
    block += name.encode("ascii") + b"\0"
    where = img.header_free(len(block), 4, avoid)
    if where is None:
        raise PatchError(f"{what}: no room in the headers for the section names "
                         f"and counters (0x{len(block):X} bytes free and zero "
                         f"before 0x{limit:X})")

    raw = _align(len(img.data), PAGE)
    payload = bytearray([fill]) * size
    payload[:len(initial)] = initial
    img.data += bytes(raw - len(img.data))
    img.data += payload

    if table_new != table:
        img.data[table_new:table_new + count * SECTHDR_SIZE] = \
            img.data[table:table + count * SECTHDR_SIZE]
    entry = bytearray(SECTHDR_SIZE)
    flags = SECTION_PRELOAD | (SECTION_WRITABLE if op.get("writable", True) else 0)
    # The digest stays zero: nothing on this runtime checks it.
    struct.pack_into("<9I", entry, 0, flags, va, size, raw, size, 0, 0,
                     base + where + new_counters, base + where + new_counters + 2)
    img.data[table_new + count * SECTHDR_SIZE:table_new + table_bytes] = entry
    img.data[where:where + len(block)] = block
    for i in range(count + 1):
        off = table_new + i * SECTHDR_SIZE
        if i in name_at:
            img.put32(off + 0x14, base + where + name_at[i])
        if i == count:
            continue        # the new entry's counters are already its own
        for field in (0x1C, 0x20):
            ptr = img.u32(off + field) - base
            if ptr in counter_at:
                img.put32(off + field, base + where + counter_at[ptr])

    img.put32(H_SECTION_COUNT, count + 1)
    img.put32(H_SECTION_HEADERS, base + table_new)
    end = max(table_new + table_bytes, where + len(block))
    if end > hdr:
        img.put32(H_SIZE_OF_HEADERS, end)
    image_end = max(base + img.u32(H_SIZE_OF_IMAGE), va + size)
    img.put32(H_SIZE_OF_IMAGE, image_end - base)
    img.added.append(name)
    log.append(f"add_section {name}: 0x{va:08X}-0x{va + size:08X} "
               f"({'writable' if flags & SECTION_WRITABLE else 'read-only'}), "
               f"image ends 0x{image_end:08X}")


def _is_code(section, data_sections):
    import fnmatch
    if section is None or not section.flags & SECTION_EXECUTABLE:
        return False
    if section.name in DATA_SECTION_NAMES:
        return False
    return not any(fnmatch.fnmatchcase(section.name, p) for p in data_sections)


def _decode(code, addr, stop):
    """[(address, size, mnemonic, operands)] from addr until an instruction
    ends at or past stop."""
    try:
        import capstone
    except ImportError:
        raise PatchError("checking a patch in code needs the capstone package "
                         "(pip install capstone), as tools.disasm does") from None
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    out = []
    at = addr
    for insn in md.disasm(code, addr):
        if insn.address != at:
            break
        out.append((insn.address, insn.size, insn.mnemonic, insn.op_str))
        at += insn.size
        if at >= stop:
            return out
    return None     # ran into bytes that do not decode


def _check_code_patch(img, va, old, new, insn, what):
    """Same instructions, same lengths: only operands change."""
    if not (va - 15 < insn <= va):
        raise PatchError(f"{what}: insn 0x{insn:08X} must be the start of the "
                         f"instruction holding 0x{va:08X} (at most 15 bytes "
                         f"before it)")
    stop = va + len(old)
    section = img.section_at(insn)
    if section is None or getattr(img.section_at(va), "index", None) != section.index:
        raise PatchError(f"{what}: insn 0x{insn:08X} is not in the patched section")
    span = min(stop - insn + 16, section.end - insn)
    before = img.read(insn, span, what)
    head = va - insn
    after = before[:head] + new + before[head + len(new):]
    a = _decode(before, insn, stop)
    b = _decode(after, insn, stop)

    def show(lst):
        return "; ".join(f"0x{x[0]:08X} {x[2]} {x[3]}".rstrip()
                         for x in lst or []) or "(does not decode)"
    if a is None:
        raise PatchError(f"{what}: the original bytes at 0x{insn:08X} do not "
                         f"decode as instructions; give the start of the "
                         f"instruction as \"insn\", or mark data with "
                         f"\"data\": true")
    if b is None or [(x[0], x[1], x[2]) for x in a] != \
            [(x[0], x[1], x[2]) for x in b]:
        raise PatchError(f"{what}: changes the instructions, not only their "
                         f"operands.\n  before: {show(a)}\n  after:  {show(b)}\n"
                         f"  Only same-length operand and immediate changes are "
                         f"allowed in code (docs/technical/xbe-patches.md)")
    return a, b


def _patch(img, op, what, log, data_sections):
    va = _addr(img, op["va"], what + " va")
    old = _hex(op["old"], what + " old")
    new = _hex(op["new"], what + " new")
    if not old or len(old) != len(new):
        raise PatchError(f"{what}: old and new must be the same, non-zero "
                         f"length ({len(old)} and {len(new)} bytes)")
    section, _off = img._span(va, len(old), what)
    have = img.read(va, len(old), what)
    if have != old:
        raise PatchError(f"{what}: 0x{va:08X} holds {have.hex().upper()}, not "
                         f"{old.hex().upper()}: another XBE, or the patch is "
                         f"already applied")
    shown = ""
    if not op.get("data", False) and _is_code(section, data_sections):
        insn = _addr(img, op.get("insn", op["va"]), what + " insn")
        _a, b = _check_code_patch(img, va, old, new, insn, what)
        shown = " [" + "; ".join(f"{x[2]} {x[3]}".rstrip() for x in b) + "]"
    img.write(va, new, what)
    log.append(f"patch 0x{va:08X}: {old.hex().upper()} -> {new.hex().upper()}"
               f"{shown}")


def _require_added(img, va, size, what):
    s = img.section_at(va)
    if s is None or s.name not in img.added or va + size > s.end:
        raise PatchError(f"{what}: 0x{va:08X}+0x{size:X} is not inside a section "
                         f"this patch file added; change the title's own "
                         f"sections with \"patch\", which states the old bytes")


def _copy(img, op, what, log):
    src = _addr(img, op["from"], what + " from")
    dst = _addr(img, op["to"], what + " to")
    size = _int(op["size"], what + " size")
    if size <= 0:
        raise PatchError(f"{what}: size must be positive")
    _require_added(img, dst, size, what)
    img.write(dst, img.read(src, size, what), what)
    log.append(f"copy 0x{src:08X} -> 0x{dst:08X}, 0x{size:X} bytes")


def _fill(img, op, what, log):
    va = _addr(img, op["va"], what + " va")
    size = _int(op["size"], what + " size")
    value = _int(op.get("value", 0), what + " value")
    if size <= 0 or not 0 <= value <= 0xFF:
        raise PatchError(f"{what}: size must be positive and value one byte")
    _require_added(img, va, size, what)
    img.write(va, bytes([value]) * size, what)
    log.append(f"fill 0x{va:08X}, 0x{size:X} bytes of 0x{value:02X}")


def _write(img, op, what, log):
    va = _addr(img, op["va"], what + " va")
    payload = _hex(op["bytes"], what + " bytes")
    if not payload:
        raise PatchError(f"{what}: no bytes")
    _require_added(img, va, len(payload), what)
    img.write(va, payload, what)
    log.append(f"write 0x{va:08X}, {len(payload)} bytes")


def apply_patches(data: bytes, spec: dict, data_sections=(), name="patch file"):
    """Apply a loaded patch file to an XBE's bytes.

    Returns (patched bytes, [one line per operation]). Raises PatchError, with
    the operation's index, at the first one that does not apply.
    """
    want = spec.get("xbe_sha1")
    if want:
        have = hashlib.sha1(data).hexdigest()
        if have.lower() != str(want).lower():
            raise PatchError(f"{name} is for the XBE with SHA-1 {want}; this one "
                             f"is {have}")
    img = Image(data)
    log = []
    for i, op in enumerate(spec["operations"]):
        what = f"{name}: operation {i} ({op['op']})"
        kind = op["op"]
        if kind == "add_section":
            _add_section(img, op, what, log)
        elif kind == "patch":
            _patch(img, op, what, log, data_sections)
        elif kind == "copy":
            _copy(img, op, what, log)
        elif kind == "fill":
            _fill(img, op, what, log)
        elif kind == "write":
            _write(img, op, what, log)
    return bytes(img.data), log


# ── what the runtime applies ────────────────────────────────────────────────

def diff_runs(original: bytes, patched: bytes, join=8):
    """[(file offset, bytes)] that turn original into patched, where the
    patched file is read past the original's end as zeros. Runs closer than
    `join` bytes are merged."""
    if len(patched) < len(original):
        raise ValueError("a patched XBE is never shorter than the original")
    runs = []
    n = len(patched)
    block = 4096
    zeros = bytes(block)
    i = 0
    while i < n:
        if i % block == 0 and i + block <= n:
            ref = (original[i:i + block] if i + block <= len(original)
                   else (original[i:] + zeros)[:block])
            if patched[i:i + block] == ref:
                i += block
                continue
        old = original[i] if i < len(original) else 0
        if patched[i] == old:
            i += 1
            continue
        start = i
        last = i
        i += 1
        while i < n:
            old = original[i] if i < len(original) else 0
            if patched[i] != old:
                last = i
            elif i - last > join:
                break
            i += 1
        runs.append((start, patched[start:last + 1]))
        i = last + 1
    return runs


def build_blob(original: bytes, patched: bytes) -> bytes:
    """The byte string src/kernel/xbe_patches.c reads:

        "XBEP", u32 version, u32 original size, u32 patched size,
        20-byte SHA-1 of the original, 20-byte SHA-1 of the patched image,
        u32 run count, then each run: u32 file offset, u32 length, bytes.
    """
    runs = diff_runs(original, patched)
    out = bytearray(BLOB_MAGIC)
    out += struct.pack("<III", BLOB_VERSION, len(original), len(patched))
    out += hashlib.sha1(original).digest() + hashlib.sha1(patched).digest()
    out += struct.pack("<I", len(runs))
    for off, payload in runs:
        out += struct.pack("<II", off, len(payload)) + payload
    return bytes(out)


def render_c(original: bytes, patched: bytes, source="", log=()) -> str:
    """recomp_xbe_patches.c: the runtime's copy of the patches."""
    blob = build_blob(original, patched)
    runs = diff_runs(original, patched)
    lines = [
        "/* Generated by tools.recomp from an XBE patch file. Do not edit; change",
        " * the patch file and lift again (docs/technical/xbe-patches.md).",
        " *",
    ]
    if source:
        lines.append(f" * Patch file:   {source}")
    lines += [
        f" * Original XBE: SHA-1 {hashlib.sha1(original).hexdigest()}, "
        f"{len(original)} bytes",
        f" * Patched XBE:  SHA-1 {hashlib.sha1(patched).hexdigest()}, "
        f"{len(patched)} bytes",
        f" * {len(runs)} run(s), {sum(len(p) for _o, p in runs)} bytes",
        " *",
    ]
    if log:
        lines += [f" *   {line}" for line in log] + [" *"]
    lines += [
        " * src/kernel/xbe_patches.c applies these to default.xbe in memory, after",
        " * checking its SHA-1, before the loader maps it: what runs is the image",
        " * that was lifted. */",
        "",
        "static const unsigned char k_xbe_patches[] = {",
    ]
    for i in range(0, len(blob), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in blob[i:i + 16]) + ",")
    lines += [
        "};",
        "",
        "const unsigned char *recomp_xbe_patches(unsigned int *size)",
        "{",
        "    *size = (unsigned int)sizeof k_xbe_patches;",
        "    return k_xbe_patches;",
        "}",
        "",
    ]
    return "\n".join(lines)
