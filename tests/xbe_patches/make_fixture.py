"""Write the xbe_patches test's fixture: the synthetic XBE of
tools/xbe_patch/test_xbe_patch.py, its patched image, and the
recomp_xbe_patches.c the lift would generate for it.

    python make_fixture.py <out dir>

The code patches are marked as data so this needs no capstone: the C test is
about what the runtime does with the result, and the Python tests check them.
"""
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from tools.xbe_patch import apply_patches, render_c  # noqa: E402
from tools.xbe_patch.test_xbe_patch import spec, synthetic_xbe  # noqa: E402


def main() -> int:
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    s = spec()
    for op in s["operations"]:
        if op["op"] == "patch":
            op["data"] = True
    original = synthetic_xbe()
    patched, log = apply_patches(original, s, name="fixture")
    (out / "original.xbe").write_bytes(original)
    (out / "patched.xbe").write_bytes(patched)
    text = render_c(original, patched, source="tests/xbe_patches fixture", log=log)
    path = out / "recomp_xbe_patches.c"
    if not path.is_file() or path.read_text() != text:
        path.write_text(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
