# Copyright (c) 2026 WD Studios LLC.
# SPDX-License-Identifier: MIT
"""Fail on internal, console or trademark strings that must never reach the public samples.

usage: forbidden_scan.py [--extra-patterns FILE]... [ROOT]

Patterns live in forbidden.txt, one case-insensitive regex per line. A line starting with "@path " is matched against
file and folder paths only; every other pattern is matched against paths and against every line of every text file.
--extra-patterns merges more pattern files (same format): names that must not even be written into this public
repository are kept in a private file and passed in by the CI that has it.

Every hit is printed as "path:line:pattern" (line 0 for a path hit). forbidden_allow.txt holds reviewed exceptions
of two kinds:
  path:line:pattern      exactly such a line; silences only that pattern on that line. The pattern part must be the
                         text of a loaded pattern, so an entry cannot outlive the pattern it was written for.
  @identifier NAME       an identifier the code cannot avoid (a name an SDK API or shader defines). A hit is exempt
                         when the whole match lies inside a whole-identifier occurrence of NAME, in file contents and
                         in paths, so the entry does not break when lines move; a match that runs past either end of
                         NAME (by any alternative of its pattern), and the same letters inside a longer identifier, are
                         still reported. A letter, digit or underscore of any script, a '$', a zero-width joiner or
                         non-joiner, or a combining mark (Unicode category M) beside NAME makes it part of a longer
                         word. NAME must contain a match of a forbidden.txt pattern, must add at least one letter to it
                         (not only digits or underscores), must not continue it directly with a lowercase letter or
                         with an S that is not the start of a lowercase-continued word (a plural, written in capitals
                         or not, or a longer word, not a name built from parts), must not be matched whole by any
                         pattern, and must contain nothing an --extra-patterns file bans (so it cannot hide a private
                         name).

Limit of the exemption, for pattern authors. A match that lies inside an allowed identifier is also looked for in its
longer forms, because a pattern with alternatives or repeats can match more than the first form the regex engine
settles on, and a longer form may run out of the identifier. That retry matches the pattern against the line cut at
each candidate end, so a lookahead or an end anchor ($, \\b, \\B) at the end of a longer form sees the end of the cut
text, not the characters that really follow. A positive lookahead there can never be satisfied: such a longer form is
not found, and a banned phrase that begins inside an allowed identifier can go unreported. (A negative lookahead
there is always satisfied, which errs towards reporting.) So do not rely on a positive lookahead past the characters
a pattern consumes to catch a longer form. A fixed-width pattern has no longer form and is never retried; the retry of
a variable-width pattern stops at its longest possible match, so a very long line (a minified bundle) stays fast.

Exit status: 0 clean, 1 hits, 2 the check could not run (no patterns, unreadable or malformed pattern or allow file,
an allow entry naming no loaded pattern, root missing, nothing to scan). The scan fails closed: a check that looked
at nothing is not a pass.
"""
import argparse
import bisect
import os
import re
import sys
import unicodedata

import repo_files

try:  # the regex parser, only to measure how long a pattern's matches can be; it moved in Python 3.11
    from re import _parser as sre_parse
except ImportError:
    try:
        import sre_parse
    except ImportError:
        sre_parse = None

HERE = os.path.dirname(os.path.realpath(__file__))
DATA_URI = re.compile(r"data:[^;\s]+;base64,[A-Za-z0-9+/=]{64,}", re.IGNORECASE)  # encoded blobs match patterns by chance
ALLOW_ENTRY = re.compile(r"^([^:\s][^:]*):(\d+):(.+)$")
ALLOW_IDENTIFIER = re.compile(r"^@identifier\s+([A-Za-z_][A-Za-z0-9_]*)$")

class CheckError(Exception):
    """The check cannot run; main() turns it into exit status 2."""

def pattern_widths(text):
    """(fewest, most) characters a match of the pattern can have; the most is huge for an unbounded pattern.

    When the parser cannot tell, the answer is "any length", which only costs time.
    """
    if sre_parse is not None:
        try:
            return tuple(sre_parse.parse(text, re.IGNORECASE).getwidth())
        except Exception:  # an internal API: any failure means "unknown"
            pass
    return 0, sys.maxsize

class Pattern:
    def __init__(self, text, path_only):
        self.text = text
        self.path_only = path_only
        self.rx = re.compile(text, re.IGNORECASE)
        self.min_width, self.max_width = pattern_widths(text)
        self.fixed_width = self.min_width == self.max_width

def load_patterns(path):
    """(patterns, the stripped lines of the file that are patterns)."""
    try:
        with open(path, encoding="utf-8-sig") as f:
            lines = f.read().splitlines()
    except OSError as e:
        raise CheckError(f"cannot read pattern file {path}: {e}")
    patterns, pattern_lines = [], set()
    for n, line in enumerate(lines, 1):
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        path_only = s.startswith("@path ")
        try:
            patterns.append(Pattern(s[len("@path "):].strip() if path_only else s, path_only))
        except re.error as e:
            raise CheckError(f"{path}:{n}: invalid regular expression: {e}")
        pattern_lines.add(s)
    return patterns, pattern_lines

def identifier_problem(name, base, extra):
    """Why NAME may not be an allowed identifier, or None.

    An identifier entry is for an SDK name that contains a banned word, so it must contain a public (base) match,
    must not be a banned word on its own or with only digits and underscores added (Name_, Name2, __NAME__), and
    must not contain anything a private (extra) pattern bans: exempting it would hide that from the scan that has
    the private patterns.
    """
    if not any(p.rx.search(name) for p in base):
        return "no pattern in forbidden.txt matches it, so there is nothing to allow"
    if any(p.rx.fullmatch(name) for p in base + extra):
        return "it is a banned word on its own, not a longer identifier that contains one"
    if any(p.rx.search(name) for p in extra):
        return "it contains a name an --extra-patterns file bans"
    banned = [False] * len(name)
    for p in base:
        for m in p.rx.finditer(name):
            banned[m.start():m.end()] = [True] * (m.end() - m.start())
            after = name[m.end():m.end() + 2]
            if after[:1].islower():
                return "a lowercase letter follows a banned word directly (a plural or a longer word)"
            if after[:1] == "S" and not after[1:].islower():
                return ("an S follows a banned word directly and does not start a word (a plural written in capitals, "
                        "or an all-capitals name)")
    if not any(c.isalpha() for c, b in zip(name, banned) if not b):
        return "it adds only digits or underscores to a banned word"
    return None

def load_allow(path, base, extra):
    """(set of (path, line, pattern), set of allowed identifiers, the stripped lines of the file that are entries).

    base: the patterns of forbidden.txt; extra: the patterns of the --extra-patterns files.
    """
    pattern_texts = {p.text for p in base + extra}
    entries, identifiers, entry_lines = set(), set(), set()
    if not os.path.exists(path):
        return entries, identifiers, entry_lines
    with open(path, encoding="utf-8-sig") as f:
        for n, line in enumerate(f.read().splitlines(), 1):
            s = line.strip()
            if not s or s.startswith("#"):
                continue
            if s.startswith("@identifier"):
                m = ALLOW_IDENTIFIER.match(s)
                if not m:
                    raise CheckError(f"{path}:{n}: expected '@identifier NAME', NAME being one identifier "
                                     "(a letter or underscore, then letters, digits or underscores)")
                problem = identifier_problem(m.group(1), base, extra)
                if problem:
                    raise CheckError(f"{path}:{n}: '@identifier {m.group(1)}' is not allowed: {problem}")
                identifiers.add(m.group(1))
                entry_lines.add(s)
                continue
            m = ALLOW_ENTRY.match(s)
            if not m:
                raise CheckError(f"{path}:{n}: not an allow entry; expected path:line:pattern, exactly as the scan "
                                 "prints a hit, or '@identifier NAME'")
            if m.group(3) not in pattern_texts:
                raise CheckError(f"{path}:{n}: '{m.group(3)}' is not one of the loaded patterns; copy the entry from "
                                 "the scan's output")
            entries.add((m.group(1), int(m.group(2)), m.group(3)))
            entry_lines.add(s)
    return entries, identifiers, entry_lines

def is_word_char(c):
    """Whether c continues an identifier: a letter, digit or underscore of any script (what the regex class for word
    characters matches in text), a '$' (part of an identifier in JavaScript and others), a zero-width joiner or
    non-joiner (JavaScript's ID_Continue), or a combining mark, which attaches to the letter before it."""
    return c == "_" or c == "$" or c.isalnum() or c in "\u200c\u200d" or unicodedata.category(c).startswith("M")

class Spans:
    """The (start, end) spans of the whole-identifier occurrences of the allowed identifiers in one text."""

    def __init__(self, spans):
        self.spans = spans
        self.starts = [s for s, _ in spans]

    def enclosing(self, start, end):
        """The span that holds text[start:end] entirely, or None."""
        i = bisect.bisect_right(self.starts, start) - 1
        if i >= 0 and end <= self.spans[i][1]:
            return self.spans[i]
        return None

NO_SPANS = Spans([])

def identifier_spans(identifiers):
    """A function giving the Spans of the allowed identifiers in a text."""
    if not identifiers:
        return lambda text: NO_SPANS
    # Longest first: of two names where one begins the other, the shorter can never be a whole word where the longer
    # is not, because the rest of the longer name is word characters.
    rx = re.compile("|".join(map(re.escape, sorted(identifiers, key=len, reverse=True))))

    def spans_of(text):
        found = []
        for m in rx.finditer(text):
            start, end = m.span()
            if (start > 0 and is_word_char(text[start - 1])) or (end < len(text) and is_word_char(text[end])):
                continue  # inside a longer word; one name never starts inside another occurrence's word
            found.append((start, end))
        return Spans(found) if found else NO_SPANS
    return spans_of

def reported(pattern, text, spans):
    """Whether pattern matches text anywhere other than entirely inside one of spans (the allowed identifiers).

    Every start position is tried, so a match inside an identifier cannot hide one that begins there and runs on.
    At each start the pattern's other alternatives are tried too: search() returns the first alternative that fits,
    which may be a short one inside the identifier while a longer one runs out of it. The text itself is not altered:
    a phrase that runs into an identifier is still seen whole, and an identifier cannot join the words on either side
    of it into a match.
    """
    rx = pattern.rx
    pos = 0
    while True:
        m = rx.search(text, pos)
        if m is None:
            return False
        enclosing = spans.enclosing(m.start(), m.end())
        if enclosing is None:
            return True
        # A longer match that starts here is outside the identifier if it ends after it. Only a variable-width
        # pattern has one, and none is longer than the pattern's widest match. endpos cuts the text, so a lookahead
        # at the very end of a longer alternative sees the end of the text instead of what follows (see the header).
        # Only hits inside an allowed identifier get here.
        if not pattern.fixed_width:
            last = min(len(text), m.start() + pattern.max_width)
            if any(rx.fullmatch(text, m.start(), end) for end in range(enclosing[1] + 1, last + 1)):
                return True
        pos = m.start() + 1

def path_hits(rel, patterns, matched_by_prefix, spans_of):
    """Patterns that match rel or one of its folders, each reported once at the outermost match."""
    hits, parts = [], rel.split("/")
    for k in range(1, len(parts) + 1):
        prefix = "/".join(parts[:k])
        if prefix in matched_by_prefix:
            continue
        parent = matched_by_prefix.get("/".join(parts[:k - 1]), frozenset())
        spans = spans_of(prefix)
        matched = frozenset(p.text for p in patterns if reported(p, prefix, spans))
        matched_by_prefix[prefix] = matched
        hits.extend((prefix, text) for text in sorted(matched - parent))
    return hits

def scan(root, patterns, allow, identifiers, exempt_lines):
    """exempt_lines: {normalised real path: lines of that file that are patterns or allow entries}."""
    content = [p for p in patterns if not p.path_only]
    prefilter = re.compile("|".join(f"(?:{p.rx.pattern})" for p in content), re.IGNORECASE) if content else None
    spans_of = identifier_spans(identifiers)
    hits, matched_by_prefix, files = [], {}, repo_files.list_files(root)
    if not files:
        raise CheckError(f"no files to scan under {root}")
    for rel in files:
        hits.extend((p, 0, text) for p, text in path_hits(rel, patterns, matched_by_prefix, spans_of))
        full = os.path.join(root, rel)
        text_like = repo_files.is_text_name(os.path.basename(rel))
        try:
            with open(full, "rb") as f:
                head = f.read(8192)
                if repo_files.decode(head, text_like) is None:
                    continue  # binary: do not read a large binary into memory
                text = repo_files.decode(head + f.read(), text_like)
        except OSError as e:
            raise CheckError(f"cannot read {rel}: {e}")
        if prefilter is None:
            continue
        text = DATA_URI.sub("data:;base64,", text)
        if not prefilter.search(text):
            continue
        exempt = exempt_lines.get(os.path.normcase(os.path.realpath(full)), ())
        for n, line in enumerate(text.split("\n"), 1):
            if line.strip() in exempt:
                continue
            spans = spans_of(line)  # an identifier never spans a line
            hits.extend((rel, n, p.text) for p in content if reported(p, line, spans))
    return sorted(h for h in set(hits) if h not in allow)

def run(root, extra_patterns):
    if not os.path.isdir(root):
        raise CheckError(f"root {root} is not a directory")
    base, extra, exempt_lines = [], [], {}
    for pf, into in [(os.path.join(HERE, "forbidden.txt"), base)] + [(pf, extra) for pf in extra_patterns]:
        loaded, lines = load_patterns(pf)
        into.extend(loaded)
        exempt_lines[os.path.normcase(os.path.realpath(pf))] = lines
    if not base and not extra:
        raise CheckError("no patterns loaded; an empty pattern list would pass everything")
    allow_path = os.path.join(HERE, "forbidden_allow.txt")
    allow, identifiers, allow_lines = load_allow(allow_path, base, extra)
    exempt_lines[os.path.normcase(os.path.realpath(allow_path))] = allow_lines
    return scan(root, base + extra, allow, identifiers, exempt_lines)

def main(argv):
    ap = argparse.ArgumentParser(description="Fail on strings that must never reach the public samples.")
    ap.add_argument("root", nargs="?", default=".")
    ap.add_argument("--extra-patterns", action="append", default=[], metavar="FILE",
                    help="another pattern file, merged with forbidden.txt (repeatable)")
    args = ap.parse_args(argv)
    try:
        hits = run(args.root, args.extra_patterns)
    except CheckError as e:
        print(f"forbidden_scan: {e}", file=sys.stderr)
        return 2
    for path, line, pattern in hits:
        print(f"{path}:{line}:{pattern}")
    return 1 if hits else 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
