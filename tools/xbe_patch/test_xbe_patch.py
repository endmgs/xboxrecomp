"""XBE patches (docs/technical/xbe-patches.md) on a synthetic XBE.

A table in .rdata and an instruction indexing it: the table is copied into an
added section, the instruction repointed at the copy and a count compared
against it raised. The patched image must parse, each refusal must refuse,
the runtime's byte runs must rebuild the patched image exactly, and the lift
must emit the new address.
"""
import hashlib
import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

from tools.xbe_patch.patcher import (Image, PatchError, apply_patches,
                                     build_blob, diff_runs, render_c)

REPO = pathlib.Path(__file__).resolve().parent.parent.parent

try:
    import capstone  # noqa: F401
    HAVE_CAPSTONE = True
except ImportError:
    HAVE_CAPSTONE = False


def _make_test_xbe():
    spec = importlib.util.spec_from_file_location(
        "make_test_xbe", REPO / "scripts" / "make_test_xbe.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TEXT_VA, TEXT_RAW = 0x00011000, 0x1000
TABLE_VA = 0x00019000           # .rdata, the first section after .text
# The entry function: index the table, compare the index against its count.
#   mov eax, [ecx*8 + TABLE]   8B 04 CD <disp32>
#   cmp ecx, 0x7F              83 F9 7F
#   ret                        C3
CODE = (b"\x8B\x04\xCD" + struct.pack("<I", TABLE_VA) + b"\x83\xF9\x7F\xC3")
TABLE = bytes(range(1, 0x41))


def synthetic_xbe():
    xbe = bytearray(_make_test_xbe().build(text_size=0x8000))
    xbe[TEXT_RAW:TEXT_RAW + len(CODE)] = CODE
    rdata_raw = TEXT_RAW + 0x8000
    xbe[rdata_raw:rdata_raw + len(TABLE)] = TABLE
    # .data (0x1A000) twice the size of its bytes in the file: the second
    # half is BSS, as most of a retail title's .data is.
    struct.pack_into("<I", xbe, 0x600 + 2 * 56 + 8, 0x2000)
    return bytes(xbe)


NEW_VA = 0x0001C000             # after .data (0x1A000-0x1C000), page-aligned


def spec(**over):
    ops = [
        {"op": "add_section", "name": "TABLES", "size": "0x1000", "va": NEW_VA},
        {"op": "copy", "from": TABLE_VA, "to": "TABLES", "size": len(TABLE)},
        {"op": "copy", "from": "0x0001B800", "to": "TABLES+0x100", "size": 0x20,
         "why": "from .data, past its raw bytes: copies zeros as loaded"},
        {"op": "patch", "va": TEXT_VA + 3, "insn": TEXT_VA,
         "old": struct.pack("<I", TABLE_VA).hex(),
         "new": struct.pack("<I", NEW_VA).hex()},
        {"op": "patch", "va": TEXT_VA + 9, "insn": TEXT_VA + 7,
         "old": "7F", "new": "7E"},
        {"op": "fill", "va": "TABLES+0x200", "size": 4, "value": 0xAA},
        {"op": "write", "va": "TABLES+0x204", "bytes": "DEADBEEF"},
    ]
    s = {"format": 1, "operations": ops}
    s.update(over)
    return s


class Apply(unittest.TestCase):

    def setUp(self):
        self.xbe = synthetic_xbe()

    @unittest.skipUnless(HAVE_CAPSTONE, "capstone checks code patches")
    def test_patched_image_parses_and_holds_the_moved_table(self):
        out, log = apply_patches(self.xbe, spec())
        self.assertEqual(len(log), 7)
        img = Image(out)
        names = [s.name for s in img.sections()]
        self.assertEqual(names, [".text", ".rdata", ".data", "TABLES"])
        new = img.section_named("TABLES")
        self.assertEqual((new.va, new.vsize, new.rsize), (NEW_VA, 0x1000, 0x1000))
        self.assertEqual(img.read(NEW_VA, len(TABLE), "t"), TABLE)
        self.assertEqual(img.read(NEW_VA + 0x200, 8, "t"),
                         b"\xAA" * 4 + bytes.fromhex("DEADBEEF"))
        self.assertEqual(img.read(TEXT_VA + 3, 4, "t"), struct.pack("<I", NEW_VA))
        self.assertEqual(img.u32(0x10C), NEW_VA + 0x1000 - 0x10000)
        # Untouched: the original table and every byte outside the patches.
        self.assertEqual(img.read(TABLE_VA, len(TABLE), "t"), TABLE)

        # The toolkit's own parser reads it, new section included.
        from tools.xbe_parser.xbe_parser import XBEParser
        with tempfile.TemporaryDirectory() as d:
            path = pathlib.Path(d) / "default.xbe"
            path.write_bytes(out)
            parsed = XBEParser(str(path)).parse()
            self.assertEqual([s.name for s in parsed.sections][-1], "TABLES")
            self.assertEqual(parsed.sections[-1].virtual_addr, NEW_VA)
            self.assertEqual(parsed.header.entry_point, TEXT_VA)

    def test_the_input_is_not_modified(self):
        before = bytes(self.xbe)
        apply_patches(self.xbe, {"format": 1, "operations": spec()["operations"][:2]})
        self.assertEqual(self.xbe, before)

    def test_old_bytes_must_match(self):
        s = spec()
        s["operations"][3]["old"] = "00000000"
        with self.assertRaisesRegex(PatchError, "another XBE"):
            apply_patches(self.xbe, s)

    @unittest.skipUnless(HAVE_CAPSTONE, "capstone checks code patches")
    def test_an_instruction_may_not_change_length_or_kind(self):
        for new in ("909090", "3D7F00"):     # nops; cmp eax, imm16...: other insns
            s = {"format": 1, "operations": [
                {"op": "patch", "va": TEXT_VA + 7, "old": "83F97F", "new": new}]}
            with self.assertRaisesRegex(PatchError, "changes the instructions"):
                apply_patches(self.xbe, s)

    @unittest.skipUnless(HAVE_CAPSTONE, "capstone checks code patches")
    def test_a_data_patch_in_code_is_not_decoded(self):
        s = {"format": 1, "operations": [
            {"op": "patch", "va": TEXT_VA + 7, "old": "83F97F", "new": "909090",
             "data": True}]}
        out, _log = apply_patches(self.xbe, s)
        self.assertEqual(Image(out).read(TEXT_VA + 7, 3, "t"), b"\x90" * 3)

    def test_unchecked_writes_only_into_added_sections(self):
        s = {"format": 1, "operations": [
            {"op": "copy", "from": TABLE_VA, "to": TABLE_VA + 0x80, "size": 4}]}
        with self.assertRaisesRegex(PatchError, "not inside a section this patch"):
            apply_patches(self.xbe, s)

    def test_expected_va_and_sha1_are_checked(self):
        with self.assertRaisesRegex(PatchError, "would be placed at"):
            apply_patches(self.xbe, {"format": 1, "operations": [
                {"op": "add_section", "name": "X", "size": 16, "va": "0x00100000"}]})
        with self.assertRaisesRegex(PatchError, "SHA-1"):
            apply_patches(self.xbe, {"format": 1, "xbe_sha1": "0" * 40,
                                     "operations": []})

    def test_two_added_sections_and_several_tables(self):
        s = {"format": 1, "operations": [
            {"op": "add_section", "name": "A", "size": 0x2000},
            {"op": "add_section", "name": "B", "size": 0x10, "writable": False},
            {"op": "copy", "from": TABLE_VA, "to": "A", "size": 0x10},
            {"op": "copy", "from": TABLE_VA + 0x10, "to": "A+0x1800", "size": 0x10},
        ]}
        out, _log = apply_patches(self.xbe, s)
        img = Image(out)
        a, b = img.section_named("A"), img.section_named("B")
        self.assertEqual((a.va, b.va), (NEW_VA, NEW_VA + 0x2000))
        self.assertEqual(b.flags & 1, 0)
        self.assertEqual(img.read(a.va + 0x1800, 0x10, "t"), TABLE[0x10:0x20])


class RuntimeCopy(unittest.TestCase):
    """What src/kernel/xbe_patches.c is given rebuilds the patched image."""

    def test_runs_rebuild_the_patched_image(self):
        xbe = synthetic_xbe()
        s = {"format": 1, "operations": [
            {"op": "add_section", "name": "T", "size": 0x3000},
            {"op": "copy", "from": TABLE_VA, "to": "T+0x10", "size": 0x40},
        ]}
        out, _log = apply_patches(xbe, s)
        img = bytearray(len(out))
        img[:len(xbe)] = xbe
        for off, data in diff_runs(xbe, out):
            img[off:off + len(data)] = data
        self.assertEqual(bytes(img), out)

        blob = build_blob(xbe, out)
        self.assertEqual(blob[:4], b"XBEP")
        self.assertEqual(blob[16:36], hashlib.sha1(xbe).digest())
        self.assertEqual(blob[36:56], hashlib.sha1(out).digest())
        c = render_c(xbe, out)
        self.assertIn("const unsigned char *recomp_xbe_patches(unsigned int *size)", c)


@unittest.skipUnless(HAVE_CAPSTONE, "the pipeline needs capstone")
class LiftEmitsTheNewAddress(unittest.TestCase):
    """The whole pipeline on the synthetic XBE, with and without patches."""

    def _lift(self, d, patches):
        game = pathlib.Path(d) / "game"
        game.mkdir(exist_ok=True)
        xbe = game / "default.xbe"
        xbe.write_bytes(synthetic_xbe())
        gen = pathlib.Path(d) / "gen"
        cmd = [sys.executable, str(REPO / "scripts" / "recompile.py"), str(xbe),
               "--work-dir", str(pathlib.Path(d) / "work"), "--gen-dir", str(gen),
               "--all", "--no-seeds", "--jobs", "1"]
        if patches:
            p = pathlib.Path(d) / "patches.json"
            import json
            p.write_text(json.dumps(spec()))
            cmd += ["--xbe-patches", str(p)]
        else:
            cmd += ["--no-xbe-patches"]
        r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout[-3000:] + r.stderr[-3000:])
        self.assertEqual(xbe.read_bytes(), synthetic_xbe(), "the title's XBE changed")
        text = "".join(f.read_text() for f in sorted(gen.glob("recomp_0*.c")))
        return gen, text

    def test_lift(self):
        with tempfile.TemporaryDirectory() as d:
            gen, text = self._lift(d, patches=False)
            self.assertIn("* 8 + 0x19000)", text)
            self.assertFalse((gen / "recomp_xbe_patches.c").exists())
            before = {f.name: f.read_bytes() for f in gen.glob("*.c")}

            gen, text = self._lift(d, patches=True)
            self.assertIn("* 8 + 0x1C000)", text)
            self.assertNotIn("0x19000", text)
            self.assertIn("cmp ecx, 0x7E", text)
            self.assertTrue((gen / "recomp_xbe_patches.c").exists())

            # And back: no patch file, the very same sources as the first time.
            gen, _text = self._lift(d, patches=False)
            after = {f.name: f.read_bytes() for f in gen.glob("*.c")}
            self.assertEqual(after, before)


if __name__ == "__main__":
    unittest.main()
