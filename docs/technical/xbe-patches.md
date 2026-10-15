# XBE patches

A title sometimes needs something in its XBE to be bigger than it is: a
fixed-size table that a mod or an enhancement wants to grow. TimeSplitters 2,
the first user, keeps a 127-entry character roster at `0x002BAC60`, indexed as
`[reg*0x20 + 0x2BAC60]` in 15 functions, with the count written into the code
as immediates.

That cannot be fixed at run time. Lifted code has every address and immediate
baked in as a C constant, so the table's address and the comparisons against
its count are already compiled. The fix goes into the image **before** it is
disassembled and lifted: add a section after the image, copy the old table into
it, repoint the references and raise the counts. Then the title has to run on
that same image, although the game folder holds the original `default.xbe`.

This is toolkit work: the mechanism holds no fact about any title. A title's
patch file holds its addresses.

## Using it

```bash
python3 scripts/recompile.py game/default.xbe --work-dir <work> --project <project> \
    --xbe-patches config/<region>/xbe_patches.json
```

Without `--xbe-patches`, `recompile.py` uses `config/xbe_patches/<TITLEID>.json`
(the project's, else the toolkit's) when one exists, as it does for seeds.
`--no-xbe-patches` ignores it. A title shipped in several releases under one
title ID should pass the file explicitly, per release.

To try a patch file out without lifting:

```bash
python3 -m tools.xbe_patch check game/default.xbe xbe_patches.json
python3 -m tools.xbe_patch apply game/default.xbe xbe_patches.json -o /tmp/patched/default.xbe
```

`check` prints what each operation did (where an added section landed, the
instructions a code patch now decodes to) and writes nothing.

## The file

```json
{
  "format": 1,
  "xbe_sha1": "2809eb147385723eaa90c425be89ee4db033fc5a",
  "comment": "anything",
  "operations": [
    {"op": "add_section", "name": "ROSTER", "size": "0x2000", "va": "0x004CE000",
     "why": "room for 256 roster entries"},
    {"op": "copy", "from": "0x002BAC60", "to": "ROSTER", "size": "0xFE0"},
    {"op": "copy", "from": "0x0035C2D8", "to": "ROSTER+0x1000", "size": "0x1FC"},
    {"op": "patch", "va": "0x00012345", "insn": "0x00012342",
     "old": "60AC2B00", "new": "00E04C00",
     "why": "mov eax, [ecx + roster]: the copy"},
    {"op": "patch", "va": "0x00012400", "old": "7F", "new": "FF", "data": true},
    {"op": "fill", "va": "ROSTER+0xFE0", "size": "0x20", "value": 0},
    {"op": "write", "va": "ROSTER+0x1FFC", "bytes": "01 00 00 00"}
  ]
}
```

Numbers are JSON integers or strings (`"0x1000"`). An address is a number or
`SECTION` / `SECTION+offset`, naming any section, including one the file added
earlier. Hex bytes may contain spaces. `why` and `comment` are free text and
are encouraged: as with overrides, write the reason beside each one. Unknown
keys are refused, so a misspelt one does not silently do nothing.

| Operation | Keys | What it does |
| --- | --- | --- |
| `add_section` | `name`, `size`; optional `va`, `writable` (default true), `fill` (byte, default 0), `bytes` (hex, at its start) | A new preloaded section, placed page-aligned after the image's last section, of `size` bytes, all of them in the file. The image size grows to cover it. `va`, when given, must be where it lands: write it, so that a different XBE or a changed earlier section is refused instead of moving everything. |
| `patch` | `va`, `old`, `new`; optional `insn`, `data` | Replaces `old` with `new` (same length). Refused unless the image holds `old` there. In code, see the rules below. |
| `copy` | `from`, `to`, `size` | Copies bytes as the image is at load: a source past a section's raw size (BSS) reads as zeros. |
| `fill` | `va`, `size`; optional `value` | Fills with one byte. |
| `write` | `va`, `bytes` | Writes bytes. |

`xbe_sha1`, when present, must be the SHA-1 of the XBE given; another release
is refused before anything is applied.

### Rules

- **Operations apply in order, and the first that fails stops the lift,** with
  the operation's index and why. Nothing is written.
- **The title's own sections change only through `patch`,** which states the
  bytes it replaces. `copy`, `fill` and `write` may only write into a section
  this file added. A patch file made for another XBE (or applied to its own
  output) fails on the first `old`.
- **A `patch` in code may change operands, not instructions.** The bytes are
  decoded with capstone (as `tools.disasm` decodes them) from `insn`, which
  defaults to `va` and must be the start of the instruction holding the first
  patched byte, through the end of the patch, before and after. Every
  instruction must keep its address, length and mnemonic. So a displacement,
  an address or an immediate can change, within the room its encoding has:
  `cmp ecx, 0x7F` (`83 F9 7F`, a sign-extended imm8) can become `0x7E` but
  not `0x9F`, which needs the imm32 form, which is longer and is refused. A
  patch that changes length would move every instruction after it, which the
  lifter, the jump tables and every reference into the function would not
  follow. When a count does not fit, patch the code that uses it somewhere it
  does, or override the function in `recomp_manual.c`.
- "Code" is an executable section that is not a data section by name
  (`.data`, `.rdata`, ... and the title's `--data-sections` patterns), the same
  test the disassembler uses. Bytes that are data inside code (a jump table, a
  constant pool) take `"data": true` and are not decoded.
- **Added sections are data.** They are writable unless `"writable": false`,
  preloaded, and never executable: lifted code cannot run code that was not
  lifted, and nothing here lifts it.
- **A copy is the image at load, not at run time.** Copying a BSS table copies
  zeros; whatever the title fills in later goes where the code that fills it
  points, which is what the patches repoint.

## What happens to the image

`add_section` has to put one more section header in the XBE's headers. A
retail header has little room: TimeSplitters 2's is full to `0x1000`, with the
section names and the shared-page counters packed right behind the section
table, then the library versions, debug names, logo and a copy of the PE
headers. So the names and counters (which every section header points at, and
nothing else does) are laid out again in the header's free space, which lets
the table grow where it is; only if something that cannot move sits behind it
does the whole table move. New bytes only go where the file holds zeros, clear
of everything the header points at, and never past the first section. The
section digest of an added section is left zero: nothing on this runtime
checks digests.

## The pipeline

`scripts/recompile.py` applies the file to the title's XBE and writes the result
to `<work-dir>/xbe_patched/default.xbe` (`tools/xbe_patch/output/` without a
work dir), rewriting it only when its bytes change. **The player's default.xbe
is only ever read.** Every stage reads the patched copy, so `tools.xbe_parser`,
`tools.disasm`, `tools.func_id` and `tools.recomp` all see one image, and the
analysis JSON is written beside the patched copy (the disassembler is given its
path). The XDK symbols file is still looked for beside the title's own XBE.

The disassembly records which image it was made from
(`<work-dir>/disasm/image.json`, the image's SHA-1). A run that starts
part-way (`--from disasm` and later) with a different image -- a changed patch
file, or patches added or removed -- goes back to `parse` and says so; with
`--only` it stops instead. The same check catches a `default.xbe` replaced
under an existing work dir, with or without patches.

The lift is given the original as `--xbe-original` and writes
`recomp_xbe_patches.c` into the generated sources. Without patches it writes
nothing and removes one left by an earlier lift, so **a title without a patch
file gets byte-identical generated code** (checked on TimeSplitters 2 PAL:
`src/recomp/gen` identical before and after this change, and after a patched
lift was undone).

## The runtime

The game folder holds the unpatched `default.xbe`, and the runtime loads that.
`recomp_xbe_patches.c` holds the difference between the two images as byte runs
over the file, with the SHA-1 of both. `xbox_MemoryLayoutInit` applies them to
its in-memory copy of the file before it maps anything
(`src/kernel/xbe_patches.c`):

1. The SHA-1 of the file found must be the one the patches were made from. If
   not -- another release, another dump -- the title does not start: its code
   expects an image this file cannot become. The log says which file it wanted.
2. The runs are applied to a copy the size of the patched image.
3. The SHA-1 of the result must be the one that was lifted.

Then the loader maps it like any other XBE: the added section at its VA with
the copied tables in it, the header that names it at `0x00010000`, the larger
image size (so the runtime's low memory moves up past it if it has to), and the
patched code bytes. Code patches do not matter to the lifted code, which
already has them, but a title that reads its own code or header sees what was
lifted. The log shows `[XBE-PATCH] default.xbe patched as lifted: ...` and the
added section in the section list.

There is one implementation of the operations, the Python one; the runtime
applies its result. That is why the runtime takes byte runs and not the
operations: two implementations of "add a section" would sooner or later
disagree, and the SHA-1 checks would then stop the title.

The table reaches the runtime through `recomp_xbe_patches()`. A title lifted
without patches has no such function; a weak default (`xbe_patches_none.c`; on
MSVC an `/alternatename` in `xbe_patches.c`) returns none, and nothing changes.

## Tests

- `tools/xbe_patch/test_xbe_patch.py`: on `scripts/make_test_xbe.py`'s synthetic
  XBE, with a table in `.rdata` and an instruction indexing it -- the patched
  image parses (`tools.xbe_parser`), each refusal refuses, the runtime's byte
  runs rebuild the patched image, and the whole pipeline lifts the new address
  and raised count, then lifts the same sources as before once the patches are
  removed.
- `tests/xbe_patches` (ctest, POSIX, needs a Python to build its fixture): the
  real `xbox_MemoryLayoutInit` given the unpatched synthetic XBE and the
  generated `recomp_xbe_patches.c` maps the added section, the copied table,
  the patched code and a header identical to the patched file's, and refuses a
  file one byte different.
- TimeSplitters 2 PAL with a small patch file (a `0x1000`-byte section, `0x20`
  bytes copied into it) lifts, builds and plays the scripted run to Siberia at
  60 fps with no unresolved indirect calls.
