#!/usr/bin/env python3
"""
gen-abi.py [--exports EXPORTS] OUT LVGL_INCLUDE HEADER...

abi.h for phone-ui and its plugin apps (external.c): UI_ABI, a string of entries
`name:hash`, one per declaration of the headers (ui.h, json.h, i18n.h) and one, `lvgl`,
of all of LVGL's headers under LVGL_INCLUDE together. A plugin carries the entries it
was built with; phone-ui loads it when each of them is among its own: a declaration
added since breaks no plugin, one changed or removed does, as do other LVGL headers.

A declaration is a top-level statement of the header without its comments, its spacing
normalized: a prototype or an extern up to its `;`, a struct, union or enum with its
body, a typedef, a static inline function with its body, a #define (continued lines
joined), an #include, or a macro invoked as a declaration (LV_FONT_DECLARE(ui_font_16)).
Conditionals (#if, #ifdef, #else, #endif) are no entries themselves but part of each
declaration under them: `_` defined under both branches of #ifdef UI_APP_CATALOG is one
entry of both texts. Named: a function or variable by its name, a struct, union or enum
by `struct.tag`, a macro by its name, an include by `include.file`, a macro invocation
by its first argument; a name in several headers is one entry of all its texts. A line
that belongs to no declaration is an error: nothing is left out of the fingerprint.

EXPORTS: the names of the functions and variables those headers declare (a prototype, an
extern, LV_FONT_DECLARE()), with LVGL's (lv_*), as a list for ld --dynamic-list: what
phone-ui exports, so that a plugin calls into nothing else of it.

The files are written only when they change: what includes them would compile again otherwise.
"""

import hashlib
import os
import re
import sys

KEYWORDS = {"void", "int", "char", "short", "long", "unsigned", "signed", "float", "double",
            "bool", "const", "static", "inline", "extern", "struct", "enum", "union",
            "volatile", "typedef"}


def fail(path, line, message):
    sys.stderr.write(f"gen-abi.py: {path}:{line}: {message}\n")
    sys.exit(1)


def strip_comments(text):
    """The text without its comments, each block comment's newlines kept (line numbers)"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and text.startswith("/*", i):
            end = text.find("*/", i + 2)
            if end < 0:
                end = n
            out.append(" " + "\n" * text.count("\n", i, end))
            i = end + 2
        elif c == "/" and text.startswith("//", i):
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif c in "\"'":
            end = i + 1
            while end < n and text[end] != c:
                end += 2 if text[end] == "\\" else 1
            out.append(text[i:end + 1])
            i = end + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def braces(line):
    """The net change of brace depth over the line, its literals skipped"""
    depth, i, n = 0, 0, len(line)
    while i < n:
        c = line[i]
        if c in "\"'":
            i += 1
            while i < n and line[i] != c:
                i += 2 if line[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    return depth


MACRO_CALL = re.compile(r"^(\w+)\s*\((.*)\)$")
ATTRIBUTE = re.compile(r"__attribute__\s*\(\((?:[^()]|\([^()]*\))*\)\)")


def name_of(statement):
    """What a statement declares"""
    s = ATTRIBUTE.sub("", statement).rstrip(";").strip()
    m = MACRO_CALL.match(s)
    if m and "(" not in m.group(2) and " " not in m.group(1):
        arg = re.match(r"\s*(\w+)", m.group(2))
        return arg.group(1) if arg else m.group(1)
    if s.startswith("typedef"):
        m = re.search(r"\(\s*\*\s*(\w+)\s*\)", s)
        if m:
            return m.group(1)
        return re.findall(r"\w+", re.sub(r"\[[^\]]*\]", "", s))[-1]
    m = re.match(r"(struct|union|enum)\s+(\w+)", s)
    if m:
        return f"{m.group(1)}.{m.group(2)}"
    before = re.match(r"([^(]*)\(", s)
    if before:
        words = re.findall(r"\w+", before.group(1))
        if words and words[-1] not in KEYWORDS:
            return words[-1]
        m = re.search(r"\(\s*\*\s*(\w+)", s)
        if m:
            return m.group(1)
    s = re.sub(r"\[[^\]]*\]|=.*", "", s)
    return re.findall(r"\w+", s)[-1]


def symbol_of(statement):
    """The function or variable a prototype or an extern declares (name_of() names one
    returning a struct after it), None for a struct, union or enum"""
    s = ATTRIBUTE.sub("", statement).rstrip(";").strip()
    if re.match(r"(struct|union|enum)\s+\w*\s*\{", s) or re.match(r"(struct|union|enum)\s+\w+$", s):
        return None
    before = re.match(r"([^(]*)\(", s)
    if before:
        words = re.findall(r"\w+", before.group(1))
        if words and words[-1] not in KEYWORDS:
            return words[-1]
        m = re.search(r"\(\s*\*\s*(\w+)", s)
        return m.group(1) if m else None
    words = re.findall(r"\w+", re.sub(r"\[[^\]]*\]|=.*", "", s))
    return words[-1] if words else None


def directive_name(directive, rest):
    if directive == "define":
        return re.match(r"\w+", rest).group(0)
    if directive == "include":
        return "include." + rest.strip().strip('<>"')
    return directive + "." + (re.match(r"\w*", rest.strip()).group(0))


def declarations(path, entries, symbols):
    """Adds the header's declarations to entries (name: list of texts), and the names of
    the functions and variables it declares to symbols"""
    with open(path) as f:
        lines = strip_comments(f.read()).split("\n")
    conditions = []     # the #if's the line is under, innermost last
    statement = []      # the lines of the statement being read
    depth = 0           # its brace depth
    start = 0

    def add(name, text):
        entries.setdefault(name, []).append(" ".join(conditions) + "|" + " ".join(" ".join(text).split()))

    i = 0
    while i < len(lines):
        line = lines[i]
        i += 1
        stripped = line.strip()
        if not statement and not stripped:
            continue
        if not statement and stripped.startswith("#"):
            while stripped.endswith("\\") and i < len(lines):
                stripped = stripped[:-1] + " " + lines[i].strip()
                i += 1
            m = re.match(r"#\s*(\w+)\s*(.*)", stripped)
            if not m:
                fail(path, i, "unknown directive")
            directive, rest = m.group(1), m.group(2)
            if directive in ("if", "ifdef", "ifndef"):
                conditions.append(f"{directive} {rest.strip()}")
            elif directive in ("else", "elif"):
                if not conditions:
                    fail(path, i, f"#{directive} without #if")
                conditions[-1] = f"!{conditions[-1]} {directive} {rest.strip()}".strip()
            elif directive == "endif":
                if not conditions:
                    fail(path, i, "#endif without #if")
                conditions.pop()
            else:
                add(directive_name(directive, rest), [stripped])
            continue
        if not statement:
            start = i
            m = MACRO_CALL.match(stripped)
            if m and " " not in m.group(1) and "(" not in m.group(2):
                add(name_of(stripped), [stripped])
                symbols.add(name_of(stripped))  # LV_FONT_DECLARE(ui_font_16): an object
                continue
        statement.append(line)
        depth += braces(line)
        if depth < 0:
            fail(path, i, "unexpected }")
        if depth > 0:
            continue
        text = " ".join(statement)
        first = text.split()[0]
        if stripped.endswith(";") or (stripped.endswith("}") and first not in ("struct", "union", "enum", "typedef")):
            name = name_of(text)
            add(name, statement)
            symbol = symbol_of(text) if first not in ("typedef", "static") else None
            if symbol:
                symbols.add(symbol)  # a prototype or an extern
            statement = []
    if statement:
        fail(path, start, "declaration not finished")
    if conditions:
        fail(path, len(lines), f"#{conditions[-1]} not ended")


def lvgl_hash(include):
    h = hashlib.sha256()
    headers = []
    for root, _, files in os.walk(include):
        headers += [os.path.join(root, f) for f in files if f.endswith(".h")]
    if not os.path.isdir(include):
        fail(include, 0, "no such directory")
    for path in sorted(headers):
        h.update(os.path.relpath(path, include).encode() + b"\0")
        with open(path, "rb") as f:
            h.update(f.read())
        h.update(b"\0")
    return h.hexdigest()[:8]


def write(path, text):
    """path with text, unless it has it already: what depends on it would be made again"""
    try:
        with open(path) as f:
            if f.read() == text:
                return
    except OSError:
        pass
    with open(path, "w") as f:
        f.write(text)


def main():
    args = sys.argv[1:]
    exports = None
    if args[:1] == ["--exports"] and len(args) > 1:
        exports, args = args[1], args[2:]
    if len(args) < 2:
        sys.stderr.write(__doc__)
        sys.exit(2)
    out, lvgl, headers = args[0], args[1], args[2:]
    entries = {}
    symbols = set()
    for path in headers:
        declarations(path, entries, symbols)
    hashes = {name: hashlib.sha256("\n".join(texts).encode()).hexdigest()[:8]
              for name, texts in entries.items()}
    hashes["lvgl"] = lvgl_hash(lvgl)
    names = sorted(hashes)
    write(out, "/* Generated by gen-abi.py: the headers plugin apps are built against, a fingerprint\n"
               " * per declaration and one of LVGL's (external.c checks a plugin's against them) */\n"
               "#define UI_ABI \\\n" +
               " \\\n".join(f'\t"{name}:{hashes[name]}{" " if name != names[-1] else ""}"' for name in names) +
               "\n")
    if exports:
        write(exports, "/* Generated by gen-abi.py: what phone-ui exports to plugin apps (ld --dynamic-list),\n"
                       " * the functions and variables of the headers they are built against and LVGL's */\n"
                       "{\n\tlv_*;\n\t_lv_*;\n" +
                       "".join(f"\t{name};\n" for name in sorted(symbols)) +
                       "};\n")


main()
