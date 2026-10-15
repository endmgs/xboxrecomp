#!/usr/bin/env python3
"""
Run the whole recompilation pipeline for one title.

The four stages have to run in order and each reads the previous one's output,
so running them by hand mostly means retyping the same XBE path four times and
remembering that stage 2 needs the JSON from stage 1.

    python3 scripts/recompile.py game_files/default.xbe

Defaults to --game-only. Lifting CRT and XDK code you intend to HLE away costs
compile time and debugging attention for code you will not run; switch to --all
only when you have a reason.

Stages:
    1. parse    tools.xbe_parser   header, sections, imports -> analysis JSON
    2. disasm   tools.disasm       .text -> instructions, functions, xrefs
    3. identify tools.func_id      classify CRT / RenderWare / game functions
    4. lift     tools.recomp       x86 -> C

Re-run a later stage on its own after changing something:

    python3 scripts/recompile.py game.xbe --from identify
    python3 scripts/recompile.py game.xbe --only lift --all

Working on more than one title: give each its own stage outputs and its own
project, or the second title silently lifts from the first one's disassembly
(every retail XBE is called default.xbe, and that name is all the guard checks):

    python3 scripts/recompile.py games/ts2/default.xbe \
        --work-dir games/_pipeline/ts2 --project titles/timesplitters2
"""

import argparse
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def config_file(args, kind, title):
    """config/<kind>/<TITLEID>.json: the title project's own, when --project
    names one that has it (a title kept in a repository of its own carries its
    seeds and sections with it), else the toolkit's."""
    if getattr(args, "project", None):
        own = Path(args.project).resolve() / "config" / kind / (title + ".json")
        if own.is_file():
            return own
    return REPO / "config" / kind / (title + ".json")


def xbe_title_id(xbe):
    """The title ID from an XBE certificate, as eight uppercase hex digits.

    Read here rather than imported so the driver keeps working when the XBE is
    missing (--dry-run) and so stage one is not a prerequisite for choosing
    seeds. The certificate address is at header+0x0118 and is a plain VA, the
    image base at +0x0104, and the ID at certificate+0x08.
    """
    try:
        data = Path(xbe).read_bytes()
    except OSError:
        return None
    if len(data) < 0x011C or data[:4] != b"XBEH":
        return None
    base = int.from_bytes(data[0x0104:0x0108], "little")
    cert = int.from_bytes(data[0x0118:0x011C], "little")
    off = cert - base
    if off < 0 or off + 12 > len(data):
        return None
    return "%08X" % int.from_bytes(data[off + 8:off + 12], "little")



STAGES = ["parse", "disasm", "identify", "lift"]


def prepare_patched_xbe(xbe: Path, patch_file: Path, out_dir: Path,
                        data_sections) -> Path:
    """Write the patched copy of the title's XBE the stages read, and return
    its path. Only rewritten when its bytes change. The title's own XBE is
    only read (docs/technical/xbe-patches.md)."""
    sys.path.insert(0, str(REPO))
    from tools.xbe_patch import PatchError, apply_patches, load_spec
    try:
        spec = load_spec(patch_file)
        patched, log = apply_patches(xbe.read_bytes(), spec, data_sections,
                                     name=patch_file.name)
    except PatchError as e:
        raise SystemExit(f"error: {e}")
    out = out_dir / xbe.name
    if out.resolve() == xbe.resolve():
        raise SystemExit(f"error: the patched XBE would overwrite {xbe}")
    print(f"XBE patches {patch_file}")
    for line in log:
        print(f"           {line}")
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.is_file() or out.read_bytes() != patched:
        out.write_bytes(patched)
    return out


def image_stamp_path(args) -> Path:
    """Beside the disassembly: which image the stage outputs were made from."""
    if args.work_dir:
        return Path(args.work_dir).resolve() / "disasm" / "image.json"
    return REPO / "tools" / "disasm" / "output" / "image.json"


def image_id(stage_xbe: Path, patch_file) -> dict:
    return {"xbe_sha1": hashlib.sha1(stage_xbe.read_bytes()).hexdigest(),
            "patches": str(patch_file) if patch_file else None}


def build_commands(args, xbe: Path, analysis_json: Path, stage_xbe: Path = None):
    """Return [(stage, argv)] for the stages that were selected.

    stage_xbe is the image the stages read: the patched copy when the title
    has an XBE patch file (docs/technical/xbe-patches.md), else xbe itself.
    Files kept beside the title's XBE (its XDK symbols) are still looked for
    beside xbe."""
    original = xbe
    patched = stage_xbe is not None and Path(stage_xbe) != Path(xbe)
    xbe = Path(stage_xbe) if stage_xbe is not None else xbe
    parse = [sys.executable, "-m", "tools.xbe_parser", str(xbe),
             "--json", str(analysis_json)]

    # Deliberately not --text-only. An XBE statically links its libraries into
    # their own executable sections -- D3D, DSOUND, XACTENG, XONLINE and the
    # rest -- and --text-only leaves those unswept. Their functions are still
    # detected, but their *ends* cannot be measured, because the end-finder
    # stops at the first address it has no instruction for. Burnout 2's
    # sub_00218CD0 came out 79 bytes instead of 321: truncated at its own tail
    # jump, with the pops and the ret stranded past the cut. Every call leaked
    # the 24-byte frame, and esp eventually walked out of the guest stack.
    #
    # The extra sections are a few hundred KB against a 2 MB .text, so the
    # scan costs little; --text-only trades correctness for a few seconds.
    disasm = [sys.executable, "-m", "tools.disasm", str(xbe)]
    if patched:
        # Not left to auto-detection, which looks beside the XBE only.
        disasm += ["--analysis-json", str(analysis_json)]
    if args.text_only:
        disasm.append("--text-only")
    # Every stage defaults to one shared tools/*/output directory, and the
    # recompiler's guard against mixing titles compares file names only --
    # every retail disc is default.xbe, so two titles worked on side by side
    # silently lift one game's code from the other's disassembly. --work-dir
    # gives a title its own set of stage outputs.
    work = Path(args.work_dir).resolve() if args.work_dir else None
    if work:
        disasm += ["-o", str(work / "disasm")]
    # Seeds are entry points nothing in the image references -- vtable slots
    # and indirect-call targets recovered from a run. Discovery cannot find
    # them by construction, so omitting the file here silently undoes every
    # seeding pass: the addresses stay undetected, the calls to them stay
    # unresolved, and the next run reports the very same targets as missing.
    # Picked up by default so running the pipeline never quietly discards them.
    for seed_file in args.seeds:
        disasm += ["--seed-functions", str(seed_file)]
    if getattr(args, "data_sections", None):
        disasm += ["--data-sections", ",".join(args.data_sections)]
    if args.verbose:
        disasm.append("-v")

    identify = [sys.executable, "-m", "tools.func_id", str(xbe)]
    if work:
        identify += ["--functions", str(work / "disasm" / "functions.json"),
                     "--strings", str(work / "disasm" / "strings.json"),
                     "--xrefs", str(work / "disasm" / "xrefs.json"),
                     "-o", str(work / "func_id")]
    if args.verbose:
        identify.append("-v")

    lift = [sys.executable, "-m", "tools.recomp", str(xbe)]
    lift.append("--all" if args.all else "--game-only")
    if args.split:
        lift += ["--split", str(args.split)]
    elif getattr(args, "split_lines", None):
        lift += ["--split-lines", str(args.split_lines)]
    elif getattr(args, "split_cost", None):
        lift += ["--split-cost", str(args.split_cost)]
    if getattr(args, "jobs", None):
        lift += ["--jobs", str(args.jobs)]
    if work:
        lift += ["--disasm-dir", str(work / "disasm"),
                 "--func-id-dir", str(work / "func_id"),
                 "-o", str(work / "recomp"),
                 # Per-site indirect-call targets (tools.recomp.icall_feedback
                 # merge --sites-db): this title's, not the shared default.
                 "--icall-sites", str(work / "recomp" / "icall_sites.json")]
    # A title project copied from templates/new-game: the generated sources
    # go where its CMakeLists globs them, and the functions its
    # recomp_manual.c defines by hand are left out of the generated set so
    # the two never define the same symbol. --gen-dir still wins if both
    # are given.
    project = Path(args.project).resolve() if args.project else None
    gen_dir = args.gen_dir or (str(project / "src" / "recomp" / "gen")
                               if project else None)
    if gen_dir:
        lift += ["--gen-dir", gen_dir]
    if project and (project / "src" / "recomp_manual.c").is_file():
        lift += ["--exclude-manual", str(project / "src" / "recomp_manual.c")]
    if args.game_name:
        lift += ["--game-name", args.game_name]
    if args.trace_all_entries:
        lift.append("--trace-all-entries")
    # XDK functions with a name-keyed replacement (tools/recomp/hle.py), used
    # whenever tools.xdk_symbols has been run for this XBE.
    hle_symbols = original.with_name(original.stem + "_xdk_symbols.json")
    if hle_symbols.exists():
        lift += ["--hle-symbols", str(hle_symbols)]
    # The lift writes the runtime's copy of the patches (recomp_xbe_patches.c)
    # from the difference between the two; without this it removes a stale one.
    if patched:
        lift += ["--xbe-original", str(original)]
    if args.verbose:
        lift.append("-v")

    commands = dict(zip(STAGES, [parse, disasm, identify, lift]))

    if args.only:
        selected = [args.only]
    else:
        selected = STAGES[STAGES.index(args.start):]

    return [(name, commands[name]) for name in selected]


def run(name: str, argv, dry_run: bool) -> bool:
    print(f"\n{'=' * 70}")
    print(f"  {STAGES.index(name) + 1}/4  {name}")
    print(f"{'=' * 70}")
    print("  $ " + " ".join(argv))

    if dry_run:
        return True

    start = time.time()
    result = subprocess.run(argv, cwd=REPO)
    elapsed = time.time() - start

    if result.returncode != 0:
        print(f"\n  FAILED: stage '{name}' exited {result.returncode} "
              f"after {elapsed:.1f}s", file=sys.stderr)
        return False

    print(f"\n  done in {elapsed:.1f}s")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("xbe", help="Path to the title's default.xbe")
    ap.add_argument("--seeds", action="append", default=None, metavar="FILE",
                    help="Seed file of extra entry points. Repeatable. "
                         "Defaults to config/seeds/<TITLEID>.json for "
                         "the XBE given -- the file tools.seed_from_log "
                         "writes -- when one exists; --no-seeds "
                         "disables it.")
    ap.add_argument("--no-seeds", action="store_true",
                    help="Ignore this title's seed file")
    ap.add_argument("--xbe-patches", metavar="FILE",
                    help="XBE patch file (docs/technical/xbe-patches.md): the "
                         "stages read a patched copy of the XBE, written to "
                         "<work-dir>/xbe_patched/, and the lift links the same "
                         "patches into the title for the runtime. Defaults to "
                         "config/xbe_patches/<TITLEID>.json (the project's, "
                         "else the toolkit's) when one exists")
    ap.add_argument("--no-xbe-patches", action="store_true",
                    help="Ignore this title's XBE patch file")
    ap.add_argument("--trace-all-entries", action="store_true",
                    help="Hook every function's entry so a profiled run can "
                         "report how many distinct functions it reached. That "
                         "is the frontier measure worth comparing between "
                         "iterations; run the build with RECOMP_TRACE_PROFILE "
                         "set, or use run_and_report.py --profile.")
    ap.add_argument("--text-only", action="store_true",
                    help="Disassemble only .text, skipping the statically "
                         "linked library sections. Faster, and wrong for any "
                         "title that calls into them: their functions end up "
                         "truncated at the first byte the sweep never decoded.")
    ap.add_argument("--all", action="store_true",
                    help="Lift everything, including CRT and XDK code "
                         "(default: --game-only)")
    # 250, not 1000. Each chunk compiles single-threaded at /O2, so the
    # biggest one sets a cold build's wall time and a one-function change
    # recompiles its whole chunk. Measured on TimeSplitters 2 (3 Oct 2026):
    # 8 chunks of up to 9.2 MB at 1000, 29 of up to 2.9 MB at 250; cold build
    # 221 s -> 198 s, one changed chunk 47 s -> 22-33 s, and no measurable
    # frame-time cost in-level (uncapped, P-cores: 6.5 ms either way).
    #
    # By estimated compile cost now, not function count (Oct 2026). 250
    # functions a file let file sizes follow function sizes: BLiNX's largest
    # file held 812k lines against a median of 42k, and its slowest took 27 s
    # of a build whose median file took 4. Lines are not the answer either --
    # they explain 40% of per-file compile time, labels and x87 accesses 90%
    # (tools.recomp.translator.compile_cost). A file has ~0.1 s of fixed
    # cost, so more of them is nearly free on a parallel build. 14000 units
    # was about 2 s a file with Apple clang -O3.
    ap.add_argument("--split", type=int, default=None, metavar="N",
                    help="Functions per generated .c file, instead of "
                         "--split-cost (the old default was 250)")
    ap.add_argument("--split-lines", type=int, default=None, metavar="LINES",
                    help="Lines of C per generated .c file, roughly, "
                         "instead of --split-cost")
    ap.add_argument("--split-cost", type=int, default=14000, metavar="UNITS",
                    help="Estimated compile cost per generated .c file "
                         "(default: 14000; see translator.compile_cost). "
                         "Ignored when --split or --split-lines is given")
    ap.add_argument("--jobs", "-j", type=int, metavar="N",
                    help="Processes the lift stage uses (default: one per "
                         "CPU). The generated C is the same whatever N is; "
                         "1 lifts in a single process")
    ap.add_argument("--gen-dir", metavar="DIR",
                    help="Output directory for generated sources")
    ap.add_argument("--work-dir", metavar="DIR",
                    help="Per-title directory for the stage outputs "
                         "(disasm/, func_id/, recomp/). Without it every "
                         "stage writes to the shared tools/*/output, and two "
                         "titles both called default.xbe overwrite each other.")
    ap.add_argument("--project", metavar="DIR",
                    help="A title project made from templates/new-game. "
                         "Generated sources go to DIR/src/recomp/gen and the "
                         "functions DIR/src/recomp_manual.c defines by hand "
                         "are not generated.")
    ap.add_argument("--game-name", metavar="NAME",
                    help="Name stamped into the generated-code banners")
    ap.add_argument("--json", metavar="FILE",
                    help="Where to write the stage-1 analysis JSON "
                         "(default: <xbe stem>_analysis.json beside the XBE)")
    ap.add_argument("--from", dest="start", choices=STAGES, default="parse",
                    metavar="STAGE",
                    help=f"Start from this stage: {', '.join(STAGES)}")
    ap.add_argument("--only", choices=STAGES, metavar="STAGE",
                    help="Run only this stage")
    ap.add_argument("--dry-run", action="store_true",
                    help="Print the commands without running them")
    ap.add_argument("--verbose", "-v", action="store_true")
    args = ap.parse_args()

    xbe = Path(args.xbe).resolve()

    # Seeds belong to a title, not to the toolkit.
    #
    # This used to load config/seed_functions.json for every XBE. A seed is an
    # unconditional claim that a function starts at an address, and tools.disasm
    # only refuses one that lands mid-instruction -- so Burnout 2's 214
    # addresses were applied to any other title, and each one that happened to
    # fall on an instruction boundary became a fake function start, splitting
    # the real function around it. Silently, and on a "make this work for any
    # game" branch.
    if args.no_seeds:
        args.seeds = []
    elif args.seeds is None:
        args.seeds = []
        title = xbe_title_id(xbe)
        if title:
            per_title = config_file(args, "seeds", title)
            if per_title.is_file():
                args.seeds = [per_title]
                if args.verbose:
                    print(f"seeds: {per_title.name} (title {title})")
            elif args.verbose:
                print(f"seeds: none for title {title}")
        legacy = REPO / "config" / "seed_functions.json"
        if legacy.is_file():
            print(f"warning: {legacy} is ignored; seeds are per-title now, "
                  f"in config/seeds/<TITLEID>.json", file=sys.stderr)

    # Sections that hold data although the XBE marks them executable, which
    # it does for nearly all of them. Per title, like the seeds: a name that
    # is data in one game (MDL*, MAP*) may be code in another.
    args.data_sections = []
    title = xbe_title_id(xbe)
    sec_file = config_file(args, "sections", title or "")
    if title and sec_file.is_file():
        entries = json.loads(sec_file.read_text(encoding="utf-8")).get("data", [])
        args.data_sections = [e["pattern"] for e in entries]
        print(f"data sections: {', '.join(args.data_sections)} ({sec_file.name})")

    if not args.dry_run and not xbe.is_file():
        print(f"error: XBE not found: {xbe}", file=sys.stderr)
        return 1

    # XBE patches: the stages read a patched copy, never the title's file.
    patch_file = None
    if args.xbe_patches and not args.no_xbe_patches:
        patch_file = Path(args.xbe_patches).resolve()
        if not patch_file.is_file():
            print(f"error: XBE patch file not found: {patch_file}", file=sys.stderr)
            return 1
    elif not args.no_xbe_patches and title:
        found = config_file(args, "xbe_patches", title)
        if found.is_file():
            patch_file = found
    stage_xbe = xbe
    if patch_file:
        out_dir = (Path(args.work_dir).resolve() / "xbe_patched" if args.work_dir
                   else REPO / "tools" / "xbe_patch" / "output")
        stage_xbe = (out_dir / xbe.name if args.dry_run else
                     prepare_patched_xbe(xbe, patch_file, out_dir,
                                         args.data_sections))

    # Written next to the XBE, named after it, because tools.disasm looks
    # there first and several titles may share one directory. A patched
    # image's goes beside the patched copy: it describes that image.
    analysis_json = (Path(args.json).resolve() if args.json
                     else stage_xbe.parent / f"{stage_xbe.stem}_analysis.json")

    # The stage outputs belong to one image. Starting part-way through with
    # another -- a changed patch file, or none where there was one -- would
    # lift the new image with the old disassembly, so go back to the start.
    stamp_path = image_stamp_path(args)
    current = None if args.dry_run else image_id(stage_xbe, patch_file)
    first = args.only or args.start
    if current and first != "parse" and stamp_path.is_file():
        try:
            recorded = json.loads(stamp_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            recorded = {}
        if recorded.get("xbe_sha1") != current["xbe_sha1"]:
            why = ("the XBE patches changed" if patch_file or recorded.get("patches")
                   else "the XBE changed")
            if args.only:
                print(f"error: {why} since the stage outputs were made; run "
                      f"from parse, not --only {args.only}", file=sys.stderr)
                return 1
            print(f"{why} since the stage outputs were made: starting from "
                  f"parse, not {args.start}")
            args.start = "parse"
    elif current and first != "parse" and patch_file:
        print(f"no record of which image the stage outputs were made from: "
              f"starting from parse, not {first}")
        args.start, args.only = "parse", None

    commands = build_commands(args, xbe, analysis_json, stage_xbe)

    print(f"Title    {xbe}")
    print(f"Analysis {analysis_json}")
    print(f"Scope    {'all functions' if args.all else 'game functions only'}")

    started = time.time()
    for name, argv in commands:
        if not run(name, argv, args.dry_run):
            print("\nPipeline stopped. Fix the error above and re-run with "
                  f"--from {name}", file=sys.stderr)
            return 1

    if args.dry_run:
        print("\n(dry run: nothing was executed)")
        return 0

    if commands and commands[0][0] == "parse":
        stamp_path.parent.mkdir(parents=True, exist_ok=True)
        stamp_path.write_text(json.dumps(current, indent=1) + "\n", encoding="utf-8")

    print(f"\n{'=' * 70}")
    print(f"  Pipeline complete in {time.time() - started:.1f}s")
    print(f"{'=' * 70}")
    print("\nNext: build the generated sources against the runtime libraries.")
    print("See INSTRUCTIONS.md for the build and first-run loop.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
