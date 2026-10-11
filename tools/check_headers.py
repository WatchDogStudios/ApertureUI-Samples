# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
"""Every source file must start with the SPDX MIT header, written as a comment of its own language.

usage: check_headers.py [ROOT]

The header is two comment lines near the top (after a shebang, a doctype or "@echo off"):

    Copyright (c) <year> WD Studios LLC.      (year 2026 or later)
    SPDX-License-Identifier: MIT              (MIT is the whole expression)

Exit status: 0 all files have it, 1 some do not, 2 the check could not run (root missing, no source files found).
"""
import os
import re
import sys

import repo_files

C_STYLE = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".mm", ".ts", ".tsx", ".js", ".jsx", ".mjs",
           ".cjs", ".css", ".slang", ".hlsl", ".hlsli"}
HASH_STYLE = {".cmake", ".py", ".ps1", ".sh", ".yml", ".yaml"}
KINDS = {**{e: "c" for e in C_STYLE}, **{e: "hash" for e in HASH_STYLE}, ".html": "html", ".htm": "html", ".bat": "bat"}
NAMES = {"CMakeLists.txt": "hash"}
# Third-party output that is bundled, not written here: it keeps its own licence (see THIRD_PARTY_NOTICES.md).
EXEMPT_PREFIXES = ("CitySlice/UI/dist/",)

COMMENT = {
    "c": r"(?://+|/\*+|\*+)",
    "hash": r"#+",
    "html": r"(?:<!--)?",  # the lines after the first of a multi-line comment carry no marker
    "bat": r"(?:@?rem\b|::)",
}
PREAMBLE = {
    "c": r"#!",
    "hash": r"#!",
    "html": r"\s*<!doctype\b",
    "bat": r"\s*@?echo\s+off\s*$",
}
COPYRIGHT = re.compile(r"Copyright \(c\) (\d{4}) WD Studios LLC\.?")
SPDX = "SPDX-License-Identifier: MIT"
WINDOW = 4  # comment lines considered after the preamble

def kind_of(rel: str):
    name = rel.rsplit("/", 1)[-1]
    return NAMES.get(name) or KINDS.get(os.path.splitext(name)[1].lower())

def has_header(text: str, kind: str) -> bool:
    comment = re.compile(rf"^\s*{COMMENT[kind]}\s*(.*?)\s*(?:\*+/|-->)?\s*$", re.IGNORECASE)
    preamble = re.compile(PREAMBLE[kind], re.IGNORECASE)
    lines = [l.rstrip("\r") for l in text.split("\n")[:WINDOW + 8]]
    i = 0
    while i < len(lines) and (not lines[i].strip() or preamble.match(lines[i])):
        i += 1
    found_copyright = found_spdx = False
    for line in lines[i:i + WINDOW]:
        m = comment.match(line)
        if not m:
            continue
        c = COPYRIGHT.fullmatch(m.group(1))
        found_copyright |= bool(c and int(c.group(1)) >= 2026)
        found_spdx |= m.group(1) == SPDX
    return found_copyright and found_spdx

def main(root: str) -> int:
    if not os.path.isdir(root):
        print(f"check_headers: root {root} is not a directory", file=sys.stderr)
        return 2
    bad, checked = [], 0
    for rel in repo_files.list_files(root):
        kind = kind_of(rel)
        if kind is None or rel.startswith(EXEMPT_PREFIXES):
            continue
        checked += 1
        with open(os.path.join(root, rel), "rb") as f:
            text = repo_files.decode(f.read(4096), True)
        if not has_header(text, kind):
            bad.append(rel)
    if not checked:
        print(f"check_headers: no source files under {root}; wrong root?", file=sys.stderr)
        return 2
    for b in bad:
        print(f"missing MIT header: {b}")
    return 1 if bad else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
