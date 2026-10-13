#!/usr/bin/env python3
"""Rewrite titles/<name>/src/main.c from templates/new-game/src/main.c.

The title projects in titles/ differ from the template only in three defines
(entry point, XBE path, game directory) and the banner name. Keeping them as
copies means a template fix has to be applied by hand to each; this applies it.

    py -3 scripts/regen_title_main.py burnout2 "Burnout 2" 0x000E2555 "Burnout 2 - Point of Impact"

A fifth argument names the image to boot when it is not default.xbe (a disc
whose default.xbe is only a language picker, like Gladius's):

    py -3 scripts/regen_title_main.py gladius Gladius 0x0015B2A7 Gladius gladius_eng.xbe

Or every title at once, each keeping the name, entry point and game folder its
current main.c already has:

    py -3 scripts/regen_title_main.py --all

Paths are written relative to the executable's directory,
titles/<name>/build/<Config>/, which is where scripts/run_and_report.py
starts it from. They are spelt with backslashes, as Windows does; the runtime
turns them into forward slashes elsewhere (xbox_path_normalize).
"""
import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
BS = chr(92)


def render(name, entry, game_folder, xbe_name="default.xbe"):
    rel = BS.join(["..", "..", "..", "..", "games", game_folder])
    c_rel = rel.replace(BS, BS + BS)            # as it appears inside a C string
    lines = (REPO / "templates" / "new-game" / "src" / "main.c").read_text(
        encoding="utf-8").split("\n")
    out = []
    replaced = set()
    for line in lines:
        if line.startswith("#define YOUR_GAME_ENTRY_POINT"):
            line = "#define YOUR_GAME_ENTRY_POINT   %s  /* XBE entry point VA */" % entry
            replaced.add("entry")
        elif line.startswith("#define YOUR_GAME_XBE_PATH"):
            out.append("/* Relative to the executable's own directory, "
                       "titles/<title>/build/<Config>/,")
            out.append(" * which is where scripts/run_and_report.py starts it. "
                       "Run it from there by")
            out.append(" * hand too, or the XBE is not found. */")
            line = '#define YOUR_GAME_XBE_PATH      "%s%s%s%s"' % (c_rel, BS, BS, xbe_name)
            replaced.add("xbe")
        elif line.startswith("#define YOUR_GAME_XBE_NAME"):
            line = '#define YOUR_GAME_XBE_NAME       "%s"' % xbe_name
            replaced.add("name")
        elif line.startswith("#define YOUR_GAME_DIR"):
            line = '#define YOUR_GAME_DIR            "%s"' % c_rel
            replaced.add("dir")
        out.append(line.replace("YOUR_GAME_NAME", name))
    if replaced != {"entry", "xbe", "dir", "name"}:
        raise SystemExit("template layout changed; defines not found: %s" % replaced)
    return "\n".join(out)


def current_values(main_c):
    """The banner name, entry point, game folder and XBE an existing main.c has."""
    s = main_c.read_text(encoding="utf-8")
    m_name = re.search(r"/\*\*\n \* (.*?) - Recompiled", s)
    m_entry = re.search(r"#define YOUR_GAME_ENTRY_POINT\s+(\S+)", s)
    m_dir = re.search(r'#define YOUR_GAME_DIR\s+"([^"]*)"', s)
    if not (m_name and m_entry and m_dir):
        return None
    m_xbe = re.search(r'#define YOUR_GAME_XBE_NAME\s+"([^"]*)"', s)
    folder = m_dir.group(1).replace(BS + BS, BS).split(BS)[-1]
    return (m_name.group(1), m_entry.group(1), folder,
            m_xbe.group(1) if m_xbe else "default.xbe")


def write(title_dir, text):
    dest = REPO / "titles" / title_dir / "src" / "main.c"
    # Keep the line endings the file has (most were written on Windows, with
    # CRLF), so a regeneration's diff is its content and not every line.
    crlf = dest.exists() and b"\r\n" in dest.read_bytes()
    with open(dest, "w", encoding="utf-8", newline="\r\n" if crlf else "\n") as f:
        f.write(text)
    print("wrote", dest)


def main(argv):
    if len(argv) == 2 and argv[1] == "--all":
        failed = 0
        for d in sorted((REPO / "titles").iterdir()):
            main_c = d / "src" / "main.c"
            if not main_c.exists():
                continue
            values = current_values(main_c)
            if not values:
                print("skipped %s: its main.c does not have the template's defines" % d.name)
                failed += 1
                continue
            write(d.name, render(*values))
        return 1 if failed else 0
    if len(argv) not in (5, 6):
        print(__doc__)
        return 2
    title_dir, name, entry, game_folder = argv[1:5]
    write(title_dir, render(name, entry, game_folder, *argv[5:]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
