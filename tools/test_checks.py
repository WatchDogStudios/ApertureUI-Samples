# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
#
# This file is scanned by forbidden_scan.py like every other file, so every banned word below is assembled from
# fragments at run time; no line here spells one out.
import contextlib
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
REPO = TOOLS.parent
SCRIPTS = ("check_headers.py", "forbidden_scan.py", "repo_files.py")

HEADER_CPP = "// Copyright (c) 2026 WD Studios LLC.\n// SPDX-License-Identifier: MIT\n"
HEADER_HASH = "# Copyright (c) 2026 WD Studios LLC.\n# SPDX-License-Identifier: MIT\n"

# The reports are compared as text, so the script writes UTF-8 and the test reads UTF-8 whatever the console code page or
# PYTHONIOENCODING of the machine running the tests is (a path with a non-ASCII letter is reported).
UTF8_ENV = {**os.environ, "PYTHONUTF8": "1", "PYTHONIOENCODING": "utf-8"}

def run(script, repo, *options, tools=TOOLS):
    return subprocess.run([sys.executable, str(tools / script), *options, str(repo)], capture_output=True, text=True,
                          encoding="utf-8", env=UTF8_ENV)

def write(root, rel, content=""):
    p = Path(root) / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(content, bytes):
        p.write_bytes(content)
    else:
        p.write_text(content, encoding="utf-8", newline="")
    return p

def hits(result):
    """{(path, line, pattern)} of a forbidden_scan report."""
    out = set()
    for l in result.stdout.splitlines():
        path, line, pattern = l.split(":", 2)
        out.add((path, int(line), pattern))
    return out

@contextlib.contextmanager
def sandbox(patterns, allow=None):
    """A temp dir holding a copy of the scripts with their own pattern files in tools/, and an empty repo/."""
    with tempfile.TemporaryDirectory() as d:
        tools = Path(d) / "tools"
        tools.mkdir()
        for name in SCRIPTS:
            shutil.copy(TOOLS / name, tools / name)
        (tools / "forbidden.txt").write_text(patterns, encoding="utf-8")
        if allow is not None:
            (tools / "forbidden_allow.txt").write_text(allow, encoding="utf-8")
        (Path(d) / "repo").mkdir()
        yield Path(d), tools

class HeaderCheckTest(unittest.TestCase):
    def check(self, files):
        with tempfile.TemporaryDirectory() as d:
            for rel, content in files.items():
                write(d, rel, content)
            return run("check_headers.py", d)

    def test_file_without_header_fails(self):
        r = self.check({"a.cpp": "int x;\n"})
        self.assertEqual(r.returncode, 1)
        self.assertIn("a.cpp", r.stdout)

    def test_files_with_header_pass(self):
        r = self.check({"a.cpp": HEADER_CPP + "int x;\n", "b.cmake": HEADER_HASH})
        self.assertEqual((r.returncode, r.stdout), (0, ""))

    def test_shebang_before_header_passes(self):
        self.assertEqual(self.check({"s.py": "#!/usr/bin/env python3\n" + HEADER_HASH}).returncode, 0)

    def test_header_without_spdx_line_fails(self):
        self.assertEqual(self.check({"a.h": "// Copyright (c) 2026 WD Studios LLC.\n#pragma once\n"}).returncode, 1)

    def test_spdx_expression_must_be_exactly_mit(self):
        for expr in ("MIT OR Apache-2.0", "MIT-0", "GPL-3.0-only", "mit"):
            r = self.check({"a.cpp": f"// Copyright (c) 2026 WD Studios LLC.\n// SPDX-License-Identifier: {expr}\n"})
            self.assertEqual(r.returncode, 1, expr)

    def test_year_2026_or_later(self):
        for year, rc in (("2025", 1), ("2026", 0), ("2031", 0)):
            r = self.check({"a.cpp": f"// Copyright (c) {year} WD Studios LLC.\n// SPDX-License-Identifier: MIT\n"})
            self.assertEqual(r.returncode, rc, year)

    def test_suffix_match_ignores_case(self):
        r = self.check({"A.CPP": "int x;\n", "Shader.HLSL": "float4 x;\n"})
        self.assertEqual((r.returncode, sorted(l.split(": ")[1] for l in r.stdout.splitlines())), (1, ["A.CPP", "Shader.HLSL"]))

    def test_every_listed_type_is_checked_with_its_own_comment_syntax(self):
        ok = {
            "a.html": "<!doctype html>\n<!-- Copyright (c) 2026 WD Studios LLC.\n     SPDX-License-Identifier: MIT -->\n",
            "b.htm": "<!-- Copyright (c) 2026 WD Studios LLC. -->\n<!-- SPDX-License-Identifier: MIT -->\n",
            "c.bat": "@echo off\nREM Copyright (c) 2026 WD Studios LLC.\nREM SPDX-License-Identifier: MIT\n",
            "d.sh": "#!/bin/sh\n" + HEADER_HASH,
            "e.yml": HEADER_HASH,
            "f.mjs": HEADER_CPP, "g.cjs": HEADER_CPP, "h.jsx": HEADER_CPP, "i.cc": HEADER_CPP, "j.cxx": HEADER_CPP,
            "k.hxx": HEADER_CPP, "l.mm": HEADER_CPP,
            "m.css": "/*\n * Copyright (c) 2026 WD Studios LLC.\n * SPDX-License-Identifier: MIT\n */\n",
        }
        self.assertEqual(self.check(ok).returncode, 0)
        for rel in ok:  # the same file without a header fails, so none of these types is skipped
            self.assertEqual(self.check({rel: "x\n"}).returncode, 1, rel)

    def test_wrong_comment_syntax_does_not_count(self):
        self.assertEqual(self.check({"a.cpp": HEADER_HASH}).returncode, 1)
        self.assertEqual(self.check({"a.py": HEADER_CPP}).returncode, 1)

    def test_crlf_and_bom_files_pass(self):
        crlf = HEADER_CPP.replace("\n", "\r\n") + "int x;\r\n"
        self.assertEqual(self.check({"a.cpp": crlf, "b.cpp": b"\xef\xbb\xbf" + HEADER_CPP.encode()}).returncode, 0)

    def test_cmakelists_is_checked_and_nothing_hides_in_build_folders(self):
        r = self.check({"CMakeLists.txt": "project(x)\n", "build/gen.cpp": "int generated;\n", "node_modules/a/b.js": "x\n"})
        self.assertEqual(r.returncode, 1)
        self.assertEqual(sorted(l.split(": ")[1] for l in r.stdout.splitlines()),
                         ["CMakeLists.txt", "build/gen.cpp", "node_modules/a/b.js"])

    def test_bundled_hud_output_is_exempt(self):
        self.assertEqual(self.check({"CitySlice/UI/dist/assets/app.js": "var a=1;\n", "a.cpp": HEADER_CPP}).returncode, 0)

    def test_check_that_looked_at_nothing_fails(self):
        with tempfile.TemporaryDirectory() as d:
            r = run("check_headers.py", Path(d) / "missing")
            self.assertEqual(r.returncode, 2)
            r = run("check_headers.py", d)  # an empty root
            self.assertEqual(r.returncode, 2)
            write(d, "notes.md", "no source files\n")
            self.assertEqual(run("check_headers.py", d).returncode, 2)

# Banned things, assembled from fragments (this file is itself scanned). Each must be reported by the real patterns.
BANNED = {
    "unfinished-work marker": "TO" + "DO",
    "defect marker": "FIX" + "ME",
    "agreement marker": "N" + "DA",
    "leak marker": "CONFIDEN" + "TIAL",
    "marker behind underscores": "LIST_" + "TO" + "DO_ITEMS",
    "third-party product name": "Sl" + "ug",
    "product name in an identifier": "APUI_SL" + "UG_PRIMITIVES",
    "product name in camel case": "Sl" + "ugGlyph",
    "drive e": "E" + ":/aperture/Docs/notes",
    "drive f": "F" + ":\\tmp\\pkg",
    "drive c user folder": "C" + ":\\Users\\someone",
    "any other drive": "D" + ":/builds/x",
    "unix home folder": "/ho" + "me/runner/work",
    "mac home folder": "/Us" + "ers/someone/src",
    "private key block": "-----BEGIN " + "RSA PRIVATE KEY-----",
    "cloud access key id": "AK" + "IA" + "ABCDEFGHIJKLMNOP",
    "github token": "gh" + "p_" + "a" * 36,
    "github fine grained token": "github" + "_pat_" + "a" * 22,
    "chat token": "xo" + "xb-" + "1234567890-abcdef",
    "api secret key": "s" + "k-" + "a" * 24,
    "assigned api key": 'api_' + 'key = "' + "abcdef1234567890" + '"',
    "json quoted api key": '{"api' + 'Key": "' + "abcdef1234567890" + '"}',
    "json quoted password": '{"pass' + 'word": "hunter2hunter2"}',
    "licence key": "AP" + "UI-AB12-CD34-EF56-GH78",
}

ORDINARY = (
    "int advantage = 1;\n"
    "int sluggish = 0;\n"
    "switch (mode) { default: break; }\n"
    "// https://apertureui.wdstudios.tech/portal and http://localhost:9222\n"
    "fetch('https://example.com/users/42');\n"
    "const char* p = \"C:/Program Files/Vulkan\"; const char* w = \"C:\\\\Windows\\\\System32\";\n"
    "std::string token = ReadToken();\n"
    "env: { GITHUB_TOKEN: \"${{ secrets.GITHUB_TOKEN }}\" }\n"
    "const char* k = \"APUI-XXXX-XXXX-XXXX-XXXX\"; // the documented placeholder\n"
    "getenv(\"APUI_LICENSE_KEY\"); getenv(\"APUI_PROJECT_ID\");\n"
)

class ForbiddenScanTest(unittest.TestCase):
    def test_hit_is_reported_as_path_line_pattern(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "src/x.cpp", HEADER_CPP + "// see " + BANNED["drive e"] + "\n")
            r = run("forbidden_scan.py", d)
            self.assertEqual(r.returncode, 1)
            [(path, line, pattern)] = hits(r)
            self.assertEqual((path, line), ("src/x.cpp", 3))
            self.assertIn("A-Za-z", pattern)

    def test_each_banned_category_is_caught(self):
        with tempfile.TemporaryDirectory() as d:  # one file per case, one scan: each file must appear in the report
            names = list(BANNED)
            for i, name in enumerate(names):
                write(d, f"w{i}.cpp", HEADER_CPP + f"int a; // {BANNED[name]}\n")
            flagged = {p for p, _, _ in hits(run("forbidden_scan.py", d))}
            self.assertEqual([n for i, n in enumerate(names) if f"w{i}.cpp" not in flagged], [])

    def test_ordinary_code_is_not_flagged(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "ok.cpp", HEADER_CPP + ORDINARY)
            r = run("forbidden_scan.py", d)
            self.assertEqual((r.returncode, r.stdout), (0, ""), r.stdout)

    def test_clean_tree_passes(self):
        with tempfile.TemporaryDirectory() as d:
            write(d, "z.cpp", HEADER_CPP + "int z;\n")
            self.assertEqual(run("forbidden_scan.py", d).returncode, 0)

    def test_encoded_blob_is_not_matched_but_the_same_text_outside_one_is(self):
        word = "CONFIDEN" + "TIAL"
        blob = "A" * 40 + word + "B" * 40
        with tempfile.TemporaryDirectory() as d:
            write(d, "in_blob.css", f".a {{ background: url(data:image/png;base64,{blob}); }}\n")
            write(d, "bare.css", f".a {{ background: url({blob}); }}\n")
            self.assertEqual({p for p, _, _ in hits(run("forbidden_scan.py", d))}, {"bare.css"})

    def test_every_file_and_folder_path_is_checked(self):
        reported = {  # file -> the path the report names (a banned folder is named once, not once per file)
            "Sl" + "ug/a.cpp": "Sl" + "ug",
            "src/TO" + "DO.md": "src/TO" + "DO.md",
            "src/APPlayerD3D12Context.cpp": "src/APPlayerD3D12Context.cpp",
            "src/old.bak": "src/old.bak",
        }
        with tempfile.TemporaryDirectory() as d:
            for rel in reported:
                write(d, rel, "text\n")
            flagged = {p for p, _, _ in hits(run("forbidden_scan.py", d))}
            self.assertEqual(flagged, set(reported.values()))

    def test_a_banned_folder_is_reported_once_not_per_file(self):
        folder = "Sl" + "ug"
        with tempfile.TemporaryDirectory() as d:
            for name in ("a.txt", "b.txt", "sub/c.txt"):
                write(d, f"{folder}/{name}", "text\n")
            self.assertEqual({p for p, _, _ in hits(run("forbidden_scan.py", d))}, {folder})

    def test_binary_build_outputs_and_licence_files_are_rejected_by_name(self):
        names = ["a.dll", "b.lib", "c.pdb", "d.exe", "e.obj", "f.ilk", "g.exp", "UPPER.DLL", "aperture.license",
                 "sub/aperture.license"]
        with tempfile.TemporaryDirectory() as d:
            for rel in names:
                write(d, rel, b"\x00\x01binary")
            r = run("forbidden_scan.py", d)
            self.assertEqual(({p for p, _, _ in hits(r)}, r.returncode), (set(names), 1))

    def test_gitignore_mirrors_the_rejected_files(self):
        entries = set((REPO / ".gitignore").read_text(encoding="utf-8").split())
        for pattern in ("*.dll", "*.lib", "*.pdb", "*.exe", "*.obj", "*.ilk", "*.exp", "aperture.license"):
            self.assertIn(pattern, entries)

    def test_utf16_and_utf8_bom_text_is_scanned(self):
        text = HEADER_CPP + "// " + BANNED["leak marker"] + "\n"
        with tempfile.TemporaryDirectory() as d:
            write(d, "u16le.cpp", b"\xff\xfe" + text.encode("utf-16-le"))
            write(d, "u16be.txt", b"\xfe\xff" + text.encode("utf-16-be"))
            write(d, "u8bom.h", b"\xef\xbb\xbf" + text.encode("utf-8"))
            write(d, "u32.md", b"\xff\xfe\x00\x00" + text.encode("utf-32-le"))
            self.assertEqual({p for p, _, _ in hits(run("forbidden_scan.py", d))}, {"u16le.cpp", "u16be.txt", "u8bom.h", "u32.md"})

    def test_nul_bytes_in_a_text_file_do_not_hide_it_but_a_binary_asset_is_skipped(self):
        word = BANNED["leak marker"]
        with tempfile.TemporaryDirectory() as d:
            write(d, "bomless16.cpp", ("// " + word + "\n").encode("utf-16-le"))  # no BOM, NULs between the letters
            write(d, "nul_inside.h", b"\x00\x00 // " + word.encode() + b"\n")
            write(d, "model.glb", b"\x00" + word.encode())
            self.assertEqual({p for p, _, _ in hits(run("forbidden_scan.py", d))}, {"bomless16.cpp", "nul_inside.h"})

    def test_nothing_hides_in_build_like_folders_in_a_git_repo(self):
        if not shutil.which("git"):
            self.skipTest("git not installed")
        bad = HEADER_CPP + "// " + BANNED["leak marker"] + "\n"
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(["git", "init", "-q", d], check=True)
            for rel in ("build/a.cpp", "out/b.cpp", "_deps/c.cpp", "node_modules/pkg/d.js", "src/build/e.cpp"):
                write(d, rel, bad)
            write(d, "ignored/f.cpp", bad)
            write(d, ".gitignore", "ignored/\n")
            subprocess.run(["git", "-C", d, "add", "-f", "build", "out", "_deps", "node_modules", "src"], check=True)
            flagged = {p for p, _, _ in hits(run("forbidden_scan.py", d))}
            self.assertEqual(flagged, {"build/a.cpp", "out/b.cpp", "_deps/c.cpp", "node_modules/pkg/d.js", "src/build/e.cpp"})

    def test_nothing_hides_in_build_like_folders_outside_git(self):
        bad = HEADER_CPP + "// " + BANNED["leak marker"] + "\n"
        with tempfile.TemporaryDirectory() as d:
            for rel in ("build/a.cpp", "x/out/b.cpp", "_deps/c.cpp", "node_modules/pkg/d.js"):
                write(d, rel, bad)
            self.assertEqual(len(hits(run("forbidden_scan.py", d))), 4)

    def test_pattern_and_allow_files_exempt_only_their_own_lines(self):
        # The real tools folder spells its patterns out; scanning it must stay clean, and it includes this file.
        r = run("forbidden_scan.py", TOOLS)
        self.assertEqual((r.returncode, r.stdout), (0, ""), r.stdout)

    def test_a_comment_or_extra_text_in_the_pattern_file_is_scanned(self):
        patterns = "Zorblax\n# a comment that mentions Zorblax\n"
        with sandbox(patterns, "") as (root, tools):
            r = run("forbidden_scan.py", root, tools=tools)
            self.assertEqual(hits(r), {("tools/forbidden.txt", 2, "Zorblax")})

    def test_text_hidden_in_the_allow_file_is_scanned_but_a_real_entry_is_not(self):
        allow = "# reviewed\n# a note that mentions Zorblax\nrepo/src/a.cpp:3:Zorblax\n"
        with sandbox("Zorblax\n", allow) as (root, tools):
            write(root / "repo", "src/a.cpp", HEADER_CPP + "// Zorblax\n")
            r = run("forbidden_scan.py", root, tools=tools)
            self.assertEqual(hits(r), {("tools/forbidden_allow.txt", 2, "Zorblax")})

    def test_allow_entry_silences_only_that_pattern_on_that_line(self):
        with sandbox("Zorblax\nQuuxflip\n", "src/a.cpp:3:Zorblax\n") as (root, tools):
            write(root / "repo", "src/a.cpp", HEADER_CPP + "int a; // Zorblax Quuxflip\nint b; // Zorblax\n")
            r = run("forbidden_scan.py", root / "repo", tools=tools)
            self.assertEqual(hits(r), {("src/a.cpp", 3, "Quuxflip"), ("src/a.cpp", 4, "Zorblax")})

    def test_allow_entry_for_a_path_hit_uses_line_zero(self):
        with sandbox("@path \\.secretext$\n", "key.secretext:0:\\.secretext$\n") as (root, tools):
            write(root / "repo", "key.secretext", "x\n")
            write(root / "repo", "other.secretext", "x\n")
            self.assertEqual(hits(run("forbidden_scan.py", root / "repo", tools=tools)),
                             {("other.secretext", 0, "\\.secretext$")})

    def test_allow_entry_must_name_a_loaded_pattern(self):
        with sandbox("Zorblax\n", "src/a.cpp:3:Zorbla\n") as (root, tools):  # a typo, or a pattern since removed
            write(root / "repo", "src/a.cpp", HEADER_CPP + "// Zorblax\n")
            r = run("forbidden_scan.py", root / "repo", tools=tools)
            self.assertEqual(r.returncode, 2)
            self.assertIn("forbidden_allow.txt:1", r.stderr)
        with sandbox("Zorblax\n", "src/a.cpp:3:Quuxflip\n") as (root, tools):  # loaded only with --extra-patterns
            extra = write(root, "private.txt", "Quuxflip\n")
            write(root / "repo", "src/a.cpp", HEADER_CPP + "// Quuxflip\n")
            self.assertEqual(run("forbidden_scan.py", root / "repo", tools=tools).returncode, 2)
            r = run("forbidden_scan.py", root / "repo", "--extra-patterns", str(extra), tools=tools)
            self.assertEqual((r.returncode, r.stdout), (0, ""), r.stderr)

    def test_identifier_entry_allows_that_whole_identifier_anywhere(self):
        with sandbox("Zorblax\n", "@identifier ZorblaxCore\n@identifier __ZORBLAX_CORE__\n") as (root, tools):
            repo = root / "repo"
            write(repo, "a.cpp", HEADER_CPP + 'Load("Shaders/ZorblaxCore.hlsli"); // ZorblaxCore twice\n'
                                              'Splice("//__ZORBLAX_CORE__");\n')
            write(repo, "b.cpp", HEADER_CPP + "\n\nint ZorblaxCore();\n")  # another file, another line
            write(repo, "shaders/ZorblaxCore.hlsli", "x\n")  # and a path
            write(repo, "c.cpp", HEADER_CPP + "Zorblax();\nZorblaxCoreX();\nMyZorblaxCore();\nzorblaxcore();\n"
                                              "ZorblaxCore(); Zorblax();\n")
            r = run("forbidden_scan.py", repo, tools=tools)
            # Only the exact identifier is exempt: not a longer one, another spelling, or the word beside it.
            self.assertEqual(hits(r), {("c.cpp", n, "Zorblax") for n in (3, 4, 5, 6, 7)})

    def identifier_check(self, patterns, identifier, extra=None):
        """The scan's result for an allow file holding only '@identifier <identifier>' (and a file using it)."""
        with sandbox(patterns, f"@identifier {identifier}\n") as (root, tools):
            write(root / "repo", "a.cpp", HEADER_CPP + f"int {identifier}();\n")
            options = []
            if extra is not None:
                options = ["--extra-patterns", str(write(root, "private.txt", extra))]
            return run("forbidden_scan.py", root / "repo", *options, tools=tools)

    def test_identifier_entry_must_contain_a_public_match(self):
        # Nothing in forbidden.txt bans it, so the entry could only serve to hide a private name.
        r = self.identifier_check("Zorblax\n", "QuuxflipBackend")
        self.assertEqual(r.returncode, 2)
        self.assertIn("nothing to allow", r.stderr)

    def test_identifier_entry_must_not_be_a_banned_word_on_its_own(self):
        for patterns, identifier in (("Zorblax\n", "Zorblax"), ("zorb(?!le)\n", "Zorb"), ("zorb(?!le)\n", "ZORB")):
            r = self.identifier_check(patterns, identifier)
            self.assertEqual(r.returncode, 2, identifier)
            self.assertIn("banned word on its own", r.stderr, identifier)
        self.assertEqual(self.identifier_check("zorb(?!le)\n", "ZorbCore").returncode, 0)

    def test_identifier_entry_must_add_a_letter_to_the_banned_word(self):
        # Digits and underscores alone would let the bare word through ("Name_ is ready").
        marker = "(?<![A-Za-z0-9])" + "TO" + "DO" + "(?![A-Za-z0-9])\n"
        for patterns, identifier in (("Zorblax\n", "Zorblax_"), ("Zorblax\n", "Zorblax2"), ("Zorblax\n", "__ZORBLAX__"),
                                     ("Zorblax\n", "Zorblax_Zorblax"), (marker, "TO" + "DO_"), (marker, "_" + "TO" + "DO_1")):
            r = self.identifier_check(patterns, identifier)
            self.assertEqual(r.returncode, 2, identifier)
            self.assertIn("only digits or underscores", r.stderr, identifier)
        for identifier in ("Zorblax_Core", "__ZORBLAX_CORE__", "ZorblaxV2"):
            self.assertEqual(self.identifier_check("Zorblax\n", identifier).returncode, 0, identifier)

    def test_a_match_that_runs_past_an_allowed_identifier_is_still_reported(self):
        patterns = "Zorblax\nAcme\\s+Zorb\nCore\\s*Wibble\nAcme\\s+Wibble\n"
        with sandbox(patterns, "@identifier ZorblaxCore\n") as (root, tools):
            repo = root / "repo"
            write(repo, "a.cpp", HEADER_CPP + "// Acme ZorblaxCore\n"   # runs into the identifier from the left
                                              "ZorblaxCore Wibble();\n"  # starts inside it and runs out to the right
                                              "// Acme ZorblaxCore Wibble\n"
                                              "int ZorblaxCore;\n")
            write(repo, "Acme ZorblaxCore/b.txt", "x\n")  # and across a path
            r = run("forbidden_scan.py", repo, tools=tools)
            # The identifier on its own is never reported, and it does not join "Acme" and "Wibble" into a match.
            self.assertEqual(hits(r), {("a.cpp", 3, "Acme\\s+Zorb"), ("a.cpp", 4, "Core\\s*Wibble"),
                                       ("a.cpp", 5, "Acme\\s+Zorb"), ("a.cpp", 5, "Core\\s*Wibble"),
                                       ("Acme ZorblaxCore", 0, "Acme\\s+Zorb")})

    def test_a_letter_digit_or_underscore_of_any_script_beside_an_allowed_identifier_makes_a_longer_word(self):
        with sandbox("Zorblax\n", "@identifier ZorblaxCore\n") as (root, tools):
            repo = root / "repo"
            write(repo, "a.cpp", HEADER_CPP + "int ZorblaxCore;\n"                      # 3 allowed
                                              "int éZorblaxCore;\n"                   # 4 Latin letter before
                                              "int ZorblaxCoreé;\n"                   # 5 and after
                                              "int ЖZorblaxCore日;\n"             # 6 Cyrillic and CJK
                                              "int ZorblaxCore٣;\n"                   # 7 Arabic-Indic digit
                                              "int “ZorblaxCore”;\n")            # 8 curly quotes: not word characters
            write(repo, "éZorblaxCore.hlsli", "x\n")
            write(repo, "ZorblaxCore.hlsli", "x\n")
            r = run("forbidden_scan.py", repo, tools=tools)
            self.assertEqual(hits(r), {("a.cpp", n, "Zorblax") for n in (4, 5, 6, 7)} | {("éZorblaxCore.hlsli", 0, "Zorblax")})

    def test_identifier_entry_must_not_continue_a_banned_word_with_a_lowercase_letter(self):
        # "Name" + "s" would let the plural of the banned word through wherever it is written.
        for identifier in ("Zorblaxs", "zorblaxs", "Zorblaxcore", "ZorblaxCore_Zorblaxs"):
            r = self.identifier_check("Zorblax\n", identifier)
            self.assertEqual(r.returncode, 2, identifier)
            self.assertIn("lowercase letter follows a banned word", r.stderr, identifier)
        for identifier in ("ZorblaxCore", "SetZorblaxPrimitivesEnabled", "UIZorblaxCore", "__ZORBLAX_CORE__", "Zorblax_s"):
            self.assertEqual(self.identifier_check("Zorblax\n", identifier).returncode, 0, identifier)
        real = (TOOLS / "forbidden.txt").read_text(encoding="utf-8")  # the real pattern, with the real word
        word = "Sl" + "ug"
        for identifier in (word + "s", word.lower() + "s", word.lower() + "core"):
            self.assertEqual(self.identifier_check(real, identifier).returncode, 2, identifier)
        for identifier in ("Set" + word + "PrimitivesEnabled", word + "Alpha", "UI" + word + "Core"):
            self.assertEqual(self.identifier_check(real, identifier).returncode, 0, identifier)

    def test_a_longer_alternative_that_runs_out_of_an_allowed_identifier_is_still_reported(self):
        # search() settles on the first alternative that fits: the short one, inside ZorblaxCore.
        pattern = "Zorblax|Zorblax\\w*\\s+Wibble"
        with sandbox(pattern + "\n", "@identifier ZorblaxCore\n") as (root, tools):
            repo = root / "repo"
            write(repo, "a.cpp", HEADER_CPP + "int ZorblaxCore;\n"            # 3 allowed
                                              "// ZorblaxCore Wibble\n"         # 4 the long alternative runs past it
                                              "int ZorblaxCore(Wibble);\n"      # 5 allowed: no whitespace before Wibble
                                              "int ZorblaxCore;  // Wibble\n")  # 6 allowed: a ';' in between
            write(repo, "ZorblaxCore Wibble/c.txt", "x\n")
            r = run("forbidden_scan.py", repo, tools=tools)
            self.assertEqual(hits(r), {("a.cpp", 4, pattern), ("ZorblaxCore Wibble", 0, pattern)})

    def test_identifier_entry_must_not_contain_a_private_name(self):
        self.assertEqual(self.identifier_check("Zorblax\n", "ZorblaxQuuxflip").returncode, 0)  # public scan only
        r = self.identifier_check("Zorblax\n", "ZorblaxQuuxflip", extra="Quuxflip\n")
        self.assertEqual(r.returncode, 2)
        self.assertIn("--extra-patterns", r.stderr)
        self.assertEqual(self.identifier_check("Zorblax\n", "ZorblaxCore", extra="Quuxflip\n").returncode, 0)

    def test_identifier_entry_must_be_one_identifier(self):
        for bad in ("@identifier\n", "@identifier Zorblax Core\n", "@identifier 9Zorblax\n",
                    "@identifier Zorblax.Core\n", "@identifiers Zorblax\n"):
            with sandbox("Zorblax\n", bad) as (root, tools):
                write(root / "repo", "a.cpp", "x\n")
                r = run("forbidden_scan.py", root / "repo", tools=tools)
                self.assertEqual(r.returncode, 2, bad)
                self.assertIn("@identifier NAME", r.stderr, bad)

    def test_old_allow_format_is_rejected_not_ignored(self):
        with sandbox("Zorblax\n", "src/a.cpp:3\n") as (root, tools):
            write(root / "repo", "a.cpp", "x\n")
            r = run("forbidden_scan.py", root / "repo", tools=tools)
            self.assertEqual(r.returncode, 2)
            self.assertIn("path:line:pattern", r.stderr)

    def test_extra_patterns_are_merged_with_the_public_ones(self):
        with sandbox("Zorblax\n") as (root, tools):
            extra = write(root, "private.txt", "# private names\nQuuxflip\n@path \\.hushhush$\n")
            extra2 = write(root, "private2.txt", "Wibblewob\n")
            repo = root / "repo"
            write(repo, "a.cpp", HEADER_CPP + "// Zorblax Quuxflip Wibblewob\n")
            write(repo, "b.hushhush", "x\n")
            plain = hits(run("forbidden_scan.py", repo, tools=tools))
            self.assertEqual({(p, pat) for p, _, pat in plain}, {("a.cpp", "Zorblax")})
            both = hits(run("forbidden_scan.py", repo, "--extra-patterns", str(extra), "--extra-patterns", str(extra2), tools=tools))
            self.assertEqual({(p, pat) for p, _, pat in both},
                             {("a.cpp", "Zorblax"), ("a.cpp", "Quuxflip"), ("a.cpp", "Wibblewob"), ("b.hushhush", "\\.hushhush$")})

    def test_an_extra_pattern_file_inside_the_tree_exempts_only_its_pattern_lines(self):
        with sandbox("Zorblax\n") as (root, tools):
            extra = write(root / "repo", "private.txt", "Quuxflip\n# note about Quuxflip\n")
            r = run("forbidden_scan.py", root / "repo", "--extra-patterns", str(extra), tools=tools)
            self.assertEqual(hits(r), {("private.txt", 2, "Quuxflip")})

    def test_check_that_could_not_run_fails_closed(self):
        with sandbox("# only a comment, no patterns\n") as (root, tools):
            write(root / "repo", "a.cpp", "x\n")
            self.assertEqual(run("forbidden_scan.py", root / "repo", tools=tools).returncode, 2)  # no patterns
        with sandbox("Zorblax\n") as (root, tools):
            self.assertEqual(run("forbidden_scan.py", root / "missing", tools=tools).returncode, 2)  # no such root
            self.assertEqual(run("forbidden_scan.py", root / "repo", tools=tools).returncode, 2)  # nothing to scan
            write(root / "repo", "a.cpp", "x\n")
            self.assertEqual(run("forbidden_scan.py", root / "repo", "--extra-patterns", str(root / "none.txt"), tools=tools).returncode, 2)
            bad = write(root, "bad.txt", "(unclosed\n")
            r = run("forbidden_scan.py", root / "repo", "--extra-patterns", str(bad), tools=tools)
            self.assertEqual(r.returncode, 2)
            self.assertIn("bad.txt:1", r.stderr)

class RepositoryFilesTest(unittest.TestCase):
    def test_ignore_and_attribute_files(self):
        ignore = set((REPO / ".gitignore").read_text(encoding="utf-8").split())
        for entry in ("*.user", ".vs/", "CMakeUserPresets.json", "/build*/", "out/", "node_modules/", ".vscode/", ".idea/",
                      "apertureplayer.log", "aperture_player_state.json", "aperture_player_imgui.ini"):
            self.assertIn(entry, ignore)
        attributes = (REPO / ".gitattributes").read_text(encoding="utf-8").splitlines()
        for line in ("* text=auto eol=lf", "*.bat text eol=crlf", "*.cmd text eol=crlf"):
            self.assertIn(line, attributes)

    def test_what_the_player_dropped_is_documented_where_users_look(self):
        for rel in ("README.md", "APPlayer/App.cpp"):
            text = (REPO / rel).read_text(encoding="utf-8")
            for needed in ("F1", "--no-layout-debug", "Box Model"):
                self.assertIn(needed, text, f"{rel} does not mention {needed}")

    def test_formatting_files_exist(self):
        self.assertIn("BasedOnStyle: WebKit", (REPO / ".clang-format").read_text(encoding="utf-8"))
        self.assertIn("root = true", (REPO / ".editorconfig").read_text(encoding="utf-8"))

    def test_scaffold_passes_its_own_checks(self):
        for script in ("check_headers.py", "forbidden_scan.py"):
            r = run(script, REPO)
            self.assertEqual((r.returncode, r.stdout), (0, ""), f"{script}: {r.stdout}{r.stderr}")

@unittest.skipUnless(shutil.which("gitleaks") and shutil.which("git"), "gitleaks and git are needed")
class GitleaksRuleTest(unittest.TestCase):
    def leaks(self, content):
        """gitleaks' exit status for a one-commit repository holding content in k.txt, with this repo's config."""
        with tempfile.TemporaryDirectory() as d:
            subprocess.run(["git", "init", "-q", d], check=True)
            shutil.copy(REPO / ".gitleaks.toml", Path(d) / ".gitleaks.toml")
            write(d, "k.txt", content)
            subprocess.run(["git", "-C", d, "add", "-A"], check=True, capture_output=True)
            subprocess.run(["git", "-C", d, "-c", "user.name=t", "-c", "user.email=t@example.com", "commit", "-q", "-m", "x"],
                           check=True, capture_output=True)
            return subprocess.run(["gitleaks", "git", "--config", str(Path(d) / ".gitleaks.toml"), "--redact", "--no-banner",
                                   "--exit-code", "2", d], capture_output=True, text=True).returncode

    def test_a_licence_key_is_a_leak(self):
        self.assertEqual(self.leaks("key: " + BANNED["licence key"] + "\n"), 2)

    def test_the_placeholder_key_and_the_variable_names_are_not(self):
        self.assertEqual(self.leaks("key: APUI-XXXX-XXXX-XXXX-XXXX\nexport APUI_LICENSE_KEY=...\n"), 0)

    @unittest.skipUnless((REPO / ".git").exists(), "needs the git history")
    def test_this_repository_has_no_leaks(self):
        r = subprocess.run(["gitleaks", "git", "--config", str(REPO / ".gitleaks.toml"), "--redact", "--no-banner",
                            "--exit-code", "2", str(REPO)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

if __name__ == "__main__":
    unittest.main()
