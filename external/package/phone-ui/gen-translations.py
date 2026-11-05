#!/usr/bin/env python3
"""phone-ui's translations (i18n.h) from po/<code>.po into one C file, at build time:
gen-translations.py <po directory> <output.c>

Each .po file is a language: its header's Language (the code, the setting's value) and
X-Language-Name (the language's name in itself, as Settings › Language lists it). Its
messages become { English, translation } pairs sorted by the English (strcmp() order:
i18n.c searches them), msgctxt joined to msgid with \\004 as C_() does. Left out, as
gettext does: untranslated, fuzzy and obsolete (#~) messages. English comes first, then the
languages by file name. Python's standard library only: every build environment has it.
"""

import os
import re
import sys

ESCAPES = {"n": "\n", "t": "\t", "r": "\r", "a": "\a", "b": "\b", "f": "\f", "v": "\v",
           "\\": "\\", '"': '"', "'": "'", "?": "?"}


def po_string(text, where):
    """A .po quoted string's content"""
    if not (text.startswith('"') and text.endswith('"') and len(text) >= 2):
        sys.exit(f"{where}: not a quoted string: {text}")
    out, s, i = bytearray(), text[1:-1], 0
    while i < len(s):
        c = s[i]
        if c != "\\":
            out += c.encode()
            i += 1
            continue
        n = s[i + 1:i + 2]
        if n in ESCAPES:
            out += ESCAPES[n].encode()
            i += 2
        elif re.match(r"[0-7]", n):
            m = re.match(r"[0-7]{1,3}", s[i + 1:])
            out.append(int(m.group(), 8))
            i += 1 + len(m.group())
        elif n == "x":
            m = re.match(r"[0-9a-fA-F]+", s[i + 2:])
            out.append(int(m.group(), 16) & 0xFF)
            i += 2 + len(m.group())
        else:
            sys.exit(f"{where}: unknown escape \\{n}")
    return out.decode("utf-8")


def parse(path):
    """The header's fields and the messages: [(context or None, id, translation, fuzzy)]"""
    messages, entry, field, fuzzy = [], {}, None, False

    def done():
        nonlocal entry, field, fuzzy
        if "msgid" in entry:
            messages.append((entry.get("msgctxt"), entry["msgid"], entry.get("msgstr", ""), fuzzy))
        entry, field, fuzzy = {}, None, False

    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            where = f"{path}:{n}"
            if not line:
                done()
            elif line.startswith("#~"):
                continue  # obsolete
            elif line.startswith("#,"):
                if "msgid" in entry:
                    done()
                fuzzy = fuzzy or "fuzzy" in [f.strip() for f in line[2:].split(",")]
            elif line.startswith("#"):
                if "msgid" in entry:
                    done()
            elif line.startswith('"'):
                if field is None:
                    sys.exit(f"{where}: a string without a keyword")
                entry[field] += po_string(line, where)
            else:
                keyword, _, rest = line.partition(" ")
                if keyword in ("msgid_plural",) or keyword.startswith("msgstr["):
                    sys.exit(f"{where}: plural forms are not supported yet")
                if keyword not in ("msgctxt", "msgid", "msgstr"):
                    sys.exit(f"{where}: unknown keyword {keyword}")
                if keyword in ("msgctxt", "msgid") and "msgid" in entry:
                    done()
                field = keyword
                entry[field] = po_string(rest.strip(), where)
        done()

    header = {}
    for ctx, msgid, msgstr, fuzzy in messages:
        if ctx is None and msgid == "":
            for line in msgstr.split("\n"):
                key, sep, value = line.partition(":")
                if sep:
                    header[key.strip()] = value.strip()
    return header, [m for m in messages if not (m[0] is None and m[1] == "")]


def c_string(text):
    out = []
    for b in text.encode("utf-8"):
        c = chr(b)
        if c == "\\" or c == '"':
            out.append("\\" + c)
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        elif b < 0x20 or b == 0x7F:
            out.append(f"\\{b:03o}")  # three digits: a digit after it stays a digit
        else:
            out.append(c)
    return '"' + "".join(out).encode("latin-1").decode("utf-8") + '"'


def main(po_dir, output):
    languages = []
    for name in sorted(os.listdir(po_dir)):
        if not name.endswith(".po"):
            continue
        path = os.path.join(po_dir, name)
        header, messages = parse(path)
        code, native = header.get("Language"), header.get("X-Language-Name")
        if not code or not native:
            sys.exit(f"{path}: the header needs Language and X-Language-Name")
        pairs = {}
        for ctx, msgid, msgstr, fuzzy in messages:
            if not msgstr or fuzzy:
                continue
            key = msgid if ctx is None else ctx + "\004" + msgid
            if key in pairs:
                sys.exit(f"{path}: {msgid!r} twice")
            pairs[key] = msgstr
        languages.append((code, native, sorted(pairs.items(), key=lambda p: p[0].encode("utf-8"))))

    lines = ["/* Generated by gen-translations.py from the catalogs in po/: edit those */", "",
             '#include "i18n.h"', ""]
    for code, native, pairs in languages:
        lines.append(f"static const struct ui_translation table_{code}[] = {{")
        lines += [f"\t{{ {c_string(k)}, {c_string(v)} }}," for k, v in pairs]
        lines += ["};", ""]
    lines.append("static const struct ui_language languages[] = {")
    lines.append('\t{ "en", "English", NULL, 0 },')
    for code, native, pairs in languages:
        lines.append(f"\t{{ {c_string(code)}, {c_string(native)}, table_{code}, "
                     f"sizeof(table_{code}) / sizeof(table_{code}[0]) }},")
    lines += ["};", "", "const struct ui_language *const ui_languages[] = {"]
    lines += [f"\t&languages[{i}]," for i in range(len(languages) + 1)]
    lines += ["};", f"const size_t ui_language_count = {len(languages) + 1};", ""]

    text = "\n".join(lines)
    # Rewritten only when it changes: make then rebuilds only what follows from it
    try:
        with open(output, encoding="utf-8") as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(os.path.abspath(output)), exist_ok=True)
    with open(output + ".tmp", "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(output + ".tmp", output)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
