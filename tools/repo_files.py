# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
"""Which files the public-safety checks look at, and how their bytes become text."""
import codecs
import os
import subprocess

# Extensions (and dotfiles) whose content is text even when it holds NUL bytes, for example BOM-less UTF-16.
TEXT_EXTS = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".m", ".mm", ".cs", ".java", ".rs", ".go", ".lua",
    ".cmake", ".py", ".ps1", ".bat", ".cmd", ".sh", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".css", ".scss",
    ".html", ".htm", ".svg", ".xml", ".slang", ".hlsl", ".hlsli", ".glsl", ".vert", ".frag", ".comp", ".md", ".mdx",
    ".txt", ".json", ".yml", ".yaml", ".toml", ".ini", ".cfg", ".in", ".rc", ".def", ".manifest", ".props", ".targets",
    ".vcxproj", ".sln", ".csproj", ".csv", ".tsv",
}

def is_text_name(name: str) -> bool:
    return name.startswith(".") or os.path.splitext(name)[1].lower() in TEXT_EXTS

def list_files(root: str):
    """Paths relative to root, with forward slashes, of every file a check must look at.

    In a git work tree this is what git tracks plus what it would add (so build/, out/, node_modules and _deps
    cannot hide a tracked file, while ignored build output is not the check's business). Anywhere else, or when git
    fails, it is every file under root except inside .git.
    """
    if os.path.exists(os.path.join(root, ".git")):
        try:
            out = subprocess.run(["git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=True).stdout
            names = {os.fsdecode(n) for n in out.split(b"\0") if n}
            return sorted(n for n in names if os.path.isfile(os.path.join(root, n)))
        except (OSError, subprocess.CalledProcessError):
            pass
    found = []
    for dirpath, dirs, files in os.walk(root):
        dirs[:] = [d for d in dirs if d != ".git"]
        found.extend(os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, "/") for f in files)
    return sorted(found)

def decode(data: bytes, text_like: bool):
    """The file's text, or None for a binary file.

    A BOM selects UTF-8, UTF-16 or UTF-32. NUL bytes in a file with a text extension are stripped (BOM-less UTF-16
    of ASCII text) rather than treated as a reason to skip the file; without a text extension a NUL in the first
    8 KiB marks the file as binary.
    """
    for bom, encoding in ((codecs.BOM_UTF32_LE, "utf-32"), (codecs.BOM_UTF32_BE, "utf-32"),
                          (codecs.BOM_UTF16_LE, "utf-16"), (codecs.BOM_UTF16_BE, "utf-16")):
        if data.startswith(bom):
            return data.decode(encoding, errors="replace")
    if data.startswith(codecs.BOM_UTF8):
        data = data[len(codecs.BOM_UTF8):]
    if b"\0" in (data if text_like else data[:8192]):
        if not text_like:
            return None
        data = data.replace(b"\0", b"")
    return data.decode("utf-8", errors="replace")
