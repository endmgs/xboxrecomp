"""
Apply an XBE patch file (docs/technical/xbe-patches.md).

    py -3 -m tools.xbe_patch apply game/default.xbe xbe_patches.json -o work/patched/default.xbe
    py -3 -m tools.xbe_patch check game/default.xbe xbe_patches.json
    py -3 -m tools.xbe_patch emit-c game/default.xbe work/patched/default.xbe -o gen/recomp_xbe_patches.c

scripts/recompile.py --xbe-patches does all of this itself; this is for
running the stages by hand, and for trying a patch file out. The input XBE is
only ever read.
"""

import argparse
import sys
from pathlib import Path

from .patcher import PatchError, apply_patches, load_spec, render_c


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        prog="python -m tools.xbe_patch", description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("apply", "check"):
        p = sub.add_parser(name, help="apply a patch file" if name == "apply"
                           else "apply a patch file and print what it does, writing nothing")
        p.add_argument("xbe")
        p.add_argument("patches")
        if name == "apply":
            p.add_argument("-o", "--output", required=True,
                           help="where to write the patched XBE (never the input)")
        p.add_argument("--data-sections", default="",
                       help="comma-separated fnmatch patterns of sections that "
                            "hold data although marked executable (as "
                            "tools.disasm takes them)")

    p = sub.add_parser("emit-c", help="write the runtime's copy of the patches")
    p.add_argument("original")
    p.add_argument("patched")
    p.add_argument("-o", "--output", required=True)
    args = ap.parse_args(argv)

    try:
        if args.cmd == "emit-c":
            original = Path(args.original).read_bytes()
            patched = Path(args.patched).read_bytes()
            Path(args.output).write_text(render_c(original, patched), encoding="utf-8")
            print(f"wrote {args.output}")
            return 0

        xbe = Path(args.xbe).resolve()
        spec = load_spec(args.patches)
        patterns = [p for p in args.data_sections.split(",") if p]
        patched, log = apply_patches(xbe.read_bytes(), spec, patterns,
                                     name=Path(args.patches).name)
        for line in log:
            print(f"  {line}")
        if args.cmd == "apply":
            out = Path(args.output).resolve()
            if out == xbe:
                print("error: refusing to overwrite the input XBE", file=sys.stderr)
                return 1
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_bytes(patched)
            print(f"wrote {out} ({len(patched)} bytes)")
        return 0
    except PatchError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
