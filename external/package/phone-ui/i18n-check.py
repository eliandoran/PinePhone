#!/usr/bin/env python3
"""Translation coverage of phone-ui's sources (src/): the string literals a user reads, per
file, marked for translation (_(), C_(), N_(), NC_(), P_(): i18n.h) or not.

A literal counts as text when it reads as words: a capitalized word ("Battery") or words
with a space between them, once its printf conversions are left out. Not text: paths,
URLs, keys (lowercase words, identifier_like, key=value), upper-case words (AT commands,
acronyms), HTTP headers, and literals inside calls that never show them: logging
(printf, fprintf, perror...), comparisons (strcmp, strstr...), files and processes,
settings' keys, SQL (IGNORED_CALLS).

What is shown but must not be translated (brand and product names, technical values)
carries NON-NLS in a comment on its line, with the reason if it helps:
    ui_set_text(settings_info_row(card, "SoC"), "Allwinner A64"); /* NON-NLS */
Lines between NON-NLS-BEGIN and NON-NLS-END comments are left out as well.

    i18n-check.py                  the table: per file, marked, not marked, percentage
    i18n-check.py --list [path...] the literals not marked, file:line, under those paths
    i18n-check.py --strict path... fails (exit 1) when one under those paths is not marked
    i18n-check.py --update         the catalogs (po/*.po) brought up to date with the sources:
                                   new texts untranslated, changed ones fuzzy (xgettext,
                                   msgmerge: gettext's tools, in the tests shell)

--app <dir> first does the same for a plugin app (apps/<name>: its .c files, its po/), whose
--strict also fails on a text marked without a translation in each catalog, or one translated
no longer in the sources (phone-ui's are tests/test_i18n.c's).
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "src")
PO = os.path.join(HERE, "po")

# Generated sources: fonts, images; the apps' (fonts, emoji names, the Markdown parser)
SKIPPED_FILES = re.compile(r"^(ui_font_.*|icons|emoji_list|splash_image|md_fonts|md4c|shortcodes)\.c$")
# Libraries kept as upstream wrote them: quirc (its error texts are logged, not shown)
SKIPPED_DIRS = {"quirc"}

MARKERS = {"_", "C_", "N_", "NC_", "P_"}

IGNORED_CALLS = {
    # logging
    "printf", "fprintf", "dprintf", "vfprintf", "perror", "puts", "fputs", "syslog",
    "LV_LOG_USER", "LV_LOG_WARN", "LV_LOG_ERROR", "LV_LOG_INFO", "LV_LOG_TRACE", "assert",
    # comparisons and parsing
    "strcmp", "strncmp", "strcasecmp", "strncasecmp", "strstr", "strcasestr", "memcmp",
    "sscanf", "fnmatch", "regcomp", "strspn", "strcspn", "strpbrk", "strtok", "strtok_r",
    # files, processes, the environment
    "fopen", "freopen", "open", "openat", "popen", "opendir", "access", "stat", "lstat",
    "unlink", "rename", "mkdir", "rmdir", "chdir", "realpath", "readlink", "symlink",
    "getenv", "setenv", "unsetenv", "putenv", "system", "execl", "execlp", "execle",
    "execv", "execvp", "dlopen", "dlsym",
    # phone-ui's settings and files
    "ui_setting_get", "ui_setting_get_int", "ui_setting_set", "ui_setting_set_int",
    "ui_read_line", "ui_read_long", "ui_write_long", "ui_write_file",
    # SQL (the plugin apps' SQLite)
    "sqlite3_exec", "sqlite3_prepare_v2", "sqlite3_prepare_v3",
}

FORMAT = re.compile(r"%(?:\d+\$)?[-+ #0']*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|q|j|z|t)?[diouxXeEfFgGaAcspn%]")


# Words that are units: "%d Mbit/s", "%ld h %ld min" are not text to translate
UNITS = {"h", "min", "s", "ms", "mA", "mAh", "V", "A", "Hz", "kHz", "MHz", "GHz", "kbit", "Mbit",
         "dBm", "KB", "MB", "GB", "TB", "dpi", "px", "x", "m", "s²"}


def is_text(s):
    """Whether a literal reads as words a user would read"""
    if s.endswith("\n"):
        return False  # a line written out (logs, a shader's source), not a label's
    t = FORMAT.sub("0", s).strip()
    if not re.search(r"[^\W\d_]{2}", t):
        return False
    if t.startswith("/") or "://" in t or re.match(r"^[\w.-]+\.\w{1,5}$", t):
        return False  # path, URL, file name
    if re.match(r"^[A-Z][\w-]*: ", t) or re.match(r"^(\+|AT\b|[A-Z][A-Z0-9]*_[A-Z0-9_]+\b)", t):
        return False  # HTTP header, AT command or reply, wpa_supplicant's commands
    words = re.findall(r"[^\W\d_][^\W_]*(?:[-'][^\W\d_]+)*", t)
    if all(w in UNITS for w in words):
        return False
    if " " not in t:
        if "_" in t or "=" in t:
            return False  # identifier, key=value
        # One word: capitalized ("Battery", "Wi-Fi"), not lowercase or upper-case ("AT", "OK")
        return bool(re.match(r"^[^\W\d_][^\W\d_A-Z]", words[0])) and words[0][0].isupper() if words else False
    # Words: not all of them upper case (AT+CMGS=..., "UTC0 EET")
    return any(not w.isupper() for w in words if len(w) > 1)


def unescape(body):
    out, i = bytearray(), 0
    raw = body.encode()
    while i < len(raw):
        c = raw[i]
        if c == 0x5C and i + 1 < len(raw):  # backslash
            n = chr(raw[i + 1])
            i += 2
            if n == "x":
                m = re.match(rb"[0-9a-fA-F]+", raw[i:])
                out.append(int(m.group(), 16) & 0xFF)
                i += len(m.group())
            elif n in "01234567":
                m = re.match(rb"[0-7]{0,2}", raw[i:])
                out.append(int(n + m.group().decode(), 8) & 0xFF)
                i += len(m.group())
            else:
                out += {"n": b"\n", "t": b"\t", "r": b"\r"}.get(n, n.encode())
        else:
            out.append(c)
            i += 1
    return out.decode("utf-8", "replace")


TOKEN = re.compile(r"""
    (?P<comment>/\*.*?\*/|//[^\n]*)
  | (?P<string>"(?:[^"\\\n]|\\.)*")
  | (?P<char>'(?:[^'\\\n]|\\.)*')
  | (?P<directive>^[ \t]*\#[ \t]*include[^\n]*)
  | (?P<ident>[A-Za-z_]\w*)
  | (?P<open>\()
  | (?P<close>\))
  | (?P<newline>\n)
  | (?P<other>.)
""", re.S | re.M | re.X)


def scan(path):
    """The text literals of a file: (line, text, marked); adjacent literals joined"""
    with open(path, encoding="utf-8", errors="replace") as f:
        src = f.read()
    # Lines left out: NON-NLS on them, or between NON-NLS-BEGIN and -END
    skipped, inside = set(), False
    for n, line in enumerate(src.split("\n"), 1):
        if "NON-NLS-BEGIN" in line:
            inside = True
        if inside or re.search(r"NON-NLS(?!-)", line):
            skipped.add(n)
        if "NON-NLS-END" in line:
            inside = False

    found = []
    line = 1
    calls = []        # the identifier before each open parenthesis (None for other ones)
    contexts = []     # whether the literal ending there was C_()'s or NC_()'s context
    last_ident = None
    pending = None    # literals being joined: [line, text]
    for m in TOKEN.finditer(src):
        kind = m.lastgroup
        if kind != "string" and kind not in ("comment", "newline") and pending:
            # C_("context", "text"): the context is not a text of its own
            if calls[-1:] in (["C_"], ["NC_"]) and not contexts[-1]:
                contexts[-1] = True
            else:
                found.append(tuple(pending))
            pending = None
        if kind == "string":
            text = unescape(m.group()[1:-1])
            if pending:
                pending[1] += text
            else:
                pending = [line, text, any(c in MARKERS for c in calls[-1:]),
                           any(c in IGNORED_CALLS for c in calls)]
        elif kind == "ident":
            last_ident = m.group()
        elif kind == "open":
            calls.append(last_ident)
            contexts.append(False)
            last_ident = None
        elif kind == "close":
            if calls:
                calls.pop()
                contexts.pop()
            last_ident = None
        elif kind != "comment" and kind != "newline":
            last_ident = None
        line += m.group().count("\n")
    if pending:
        found.append(tuple(pending))
    return [(n, text, marked) for n, text, marked, ignored in found
            if (marked or (not ignored and n not in skipped and is_text(text)))]


def files(paths):
    for root, dirs, names in os.walk(SRC):
        dirs[:] = sorted(d for d in dirs if d not in SKIPPED_DIRS)
        for name in sorted(names):
            if not name.endswith(".c") or SKIPPED_FILES.match(name):
                continue
            rel = os.path.relpath(os.path.join(root, name), SRC)
            if not paths or any(p == "." or rel == p or rel.startswith(p.rstrip("/") + "/") for p in paths):
                yield rel


def template(pot):
    """xgettext's template of the sources' marked texts into pot"""
    subprocess.run(["xgettext", "--language=C", "--from-code=UTF-8", "--keyword=_", "--keyword=N_",
                    "--keyword=C_:1c,2", "--keyword=NC_:1c,2", "--keyword=P_:1,2",
                    "--add-location=file", "--sort-by-file", "--no-wrap", "--package-name=phone-ui",
                    "-o", pot] + sorted(files([])),
                   cwd=SRC, check=True)


def catalog_parse(path):
    """gen-translations.py's reading of a catalog: (header, messages)"""
    import importlib.util
    sys.dont_write_bytecode = True  # no __pycache__ beside it
    spec =importlib.util.spec_from_file_location("gen_translations", os.path.join(HERE, "gen-translations.py"))
    gen = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gen)
    return gen.parse(path)


def complete():
    """The texts marked without a translation in a catalog, and those translated no longer
    marked: [problem]"""
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        pot = os.path.join(tmp, "app.pot")
        template(pot)
        marked = {(c, i) for c, i, p, f, z in catalog_parse(pot)[1]} if os.path.exists(pot) else set()
    for name in sorted(os.listdir(PO)) if os.path.isdir(PO) else []:
        if not name.endswith(".po"):
            continue
        path = os.path.relpath(os.path.join(PO, name))
        translated = {(c, i) for c, i, p, forms, fuzzy in catalog_parse(path)[1]
                      if not fuzzy and forms and all(forms.values())}
        problems += [f"{path}: no translation of {i!r}" for c, i in sorted(marked - translated, key=str)]
        problems += [f"{path}: {i!r} is not in the sources" for c, i in sorted(translated - marked, key=str)]
    return problems


def update():
    """xgettext's template of the sources, merged into each catalog"""
    with tempfile.TemporaryDirectory() as tmp:
        pot = os.path.join(tmp, "phone-ui.pot")
        template(pot)
        # Without its creation date: msgmerge copies it into each catalog, a change each time
        with open(pot, encoding="utf-8") as f:
            text = re.sub(r'^"(POT-Creation-Date|Report-Msgid-Bugs-To): .*\\n"\n', "", f.read(), flags=re.M)
        with open(pot, "w", encoding="utf-8") as f:
            f.write(text)
        for name in sorted(os.listdir(PO)):
            if name.endswith(".po"):
                path = os.path.join(PO, name)
                subprocess.run(["msgmerge", "--quiet", "--update", "--backup=none", "--add-location=file",
                                "--sort-by-file", "--no-wrap", path, pot], check=True)
                stats = subprocess.run(["msgfmt", "--statistics", "-o", "/dev/null", path],
                                       capture_output=True, text=True)
                print(f"{os.path.relpath(path)}: {stats.stderr.strip()}")


def main(argv):
    global SRC, PO
    app = argv[:1] == ["--app"]
    if app:
        if len(argv) < 2:
            sys.exit(__doc__)
        SRC = os.path.abspath(argv[1])
        PO = os.path.join(SRC, "po")
        argv = argv[2:]
    mode = argv[0] if argv and argv[0].startswith("--") else None
    if mode == "--update":
        return update()
    paths = [os.path.relpath(os.path.abspath(p), SRC) if os.path.exists(p) else p.rstrip("/")
             for p in (argv[1:] if mode else argv)]
    if mode == "--strict" and not paths:
        sys.exit("--strict: which paths?")
    if mode not in (None, "--list", "--strict"):
        sys.exit(__doc__)

    rows, missing = [], []
    for rel in files(paths):
        found = scan(os.path.join(SRC, rel))
        marked = sum(1 for f in found if f[2])
        rows.append((rel, marked, len(found) - marked))
        missing += [(rel, n, text) for n, text, m in found if not m]

    if mode:
        prefix = os.path.relpath(SRC)
        for rel, n, text in missing:
            print(f"{os.path.join(prefix, rel)}:{n}: {text!r}")
        if mode == "--strict":
            if missing:
                print(f"{len(missing)} texts not marked for translation (i18n.h), "
                      "or NON-NLS if they must not be", file=sys.stderr)
            problems = complete() if app else []
            for problem in problems:
                print(problem)
            sys.exit(1 if missing or problems else 0)
        return

    width = max([len(r[0]) for r in rows] + [5])
    print(f"{'File':<{width}}  Marked  Not marked  Coverage")
    total_m = total_n = 0
    for rel, m, n in sorted(rows, key=lambda r: (-(r[1] + r[2] and r[1] / (r[1] + r[2])), -r[2], r[0])):
        if m + n == 0:
            continue
        total_m, total_n = total_m + m, total_n + n
        print(f"{rel:<{width}}  {m:>6}  {n:>10}  {100 * m / (m + n):>7.0f}%")
    if total_m + total_n:
        print(f"{'Total':<{width}}  {total_m:>6}  {total_n:>10}  {100 * total_m / (total_m + total_n):>7.0f}%")


if __name__ == "__main__":
    main(sys.argv[1:])
