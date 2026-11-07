#!/usr/bin/env python3
"""The keyboard's dictionaries (correct.h) from words/<code>.txt into one C file, at
build time: gen-words.py <words directory> <output.c>

Each list is a language's words with their counts (words/README.md: where they come from,
and <code>.fix.txt, corrections by hand applied after it). A word is kept when written
with the language's letters only (LETTERS), an apostrophe or a hyphen allowed inside it,
and up to CORRECT_LETTERS_MAX letters long; cedilla ş/ţ become comma-below ș/ț. Then a
word's spelling without its diacritics is folded into it when it is no more frequent than
it. The words go into a trie: one node per letter of a prefix, children contiguous and
sorted by letter, packed in 32 bits (child index, last sibling, letter, frequency), the
root's children first; the frequency is log2(count) in quarters, so correct.c's costs are
in bits. Each letter's key on the keyboard (its base letter, none for ' and -) goes with
the alphabet. Python's standard library only.
"""

import math
import os
import re
import sys
import unicodedata
from collections import deque

LETTERS = {
    "en": "abcdefghijklmnopqrstuvwxyz'-",
    "ro": "abcdefghijklmnopqrstuvwxyzăâîșț-",
}
CEDILLA = str.maketrans("şţŞŢ", "șțȘȚ")
LETTERS_MAX = 24  # CORRECT_LETTERS_MAX
CHILD_BITS = 18
FREQ_MAX = 127


def base(letter):
    """The key a letter is typed on: itself, or the letter under its diacritic"""
    plain = unicodedata.normalize("NFD", letter)[0]
    return plain if "a" <= plain <= "z" else None


def read_list(path, letters):
    """{word: count} of the words written with these letters"""
    allowed = re.compile(f"^[{letters}]+$")
    counts = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.split()
            if len(parts) != 2:
                continue
            word = unicodedata.normalize("NFC", parts[0].lower().translate(CEDILLA))
            if (not allowed.match(word) or word[0] in "'-" or word[-1] in "'-" or
                    "''" in word or "--" in word or len(word) > LETTERS_MAX):
                continue
            counts[word] = counts.get(word, 0) + int(parts[1])
    return counts


def apply_fixes(path, counts, letters):
    """<code>.fix.txt: `old -> new` renames (counts added), `word count` sets (0 removes)"""
    allowed = re.compile(f"^[{letters}]+$")
    with open(path, encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.split("#")[0].strip()
            if not line:
                continue
            if "->" in line:
                old, new = (s.strip() for s in line.split("->"))
                if old in counts:
                    counts[new] = counts.get(new, 0) + counts.pop(old)
                word = new
            else:
                word, count = line.split()
                if int(count) == 0:
                    counts.pop(word, None)
                else:
                    counts[word] = int(count)
            if not allowed.match(word):
                sys.exit(f"{path}:{n}: {word}: not written with the language's letters")


def strip_diacritics(word):
    return "".join(base(c) or c for c in word)


def fold_plain(counts):
    """A word's spelling without diacritics into it, when it is no more frequent"""
    variants = {}  # plain spelling -> its most frequent spelling with diacritics
    for word, count in counts.items():
        plain = strip_diacritics(word)
        if plain != word and count > counts.get(variants.get(plain), 0):
            variants[plain] = word
    for plain, word in variants.items():
        if plain in counts and counts[plain] <= counts[word]:
            counts[word] += counts.pop(plain)


def frequency(count):
    return max(1, min(FREQ_MAX, round(math.log2(count) * 4)))


def build_trie(counts, letters):
    """The packed nodes: children contiguous, sorted by letter, the root's first"""
    root = {}
    for word, count in counts.items():
        node = root
        for c in word:
            node = node.setdefault(letters.index(c), [0, {}])[1]
        # the word itself: the last letter's node holds its frequency
        node = root
        for c in word[:-1]:
            node = node[letters.index(c)][1]
        node[letters.index(word[-1])][0] = frequency(count)
    nodes = []
    queue = deque()

    def emit(children):
        start = len(nodes)
        items = sorted(children.items())
        for k, (letter, (freq, sub)) in enumerate(items):
            nodes.append([0, k == len(items) - 1, letter, freq])
            queue.append((len(nodes) - 1, sub))
        return start

    emit(root)
    while queue:
        index, sub = queue.popleft()
        if sub:
            nodes[index][0] = emit(sub)
    if len(nodes) >= 1 << CHILD_BITS:
        sys.exit(f"{len(nodes)} trie nodes: more than {CHILD_BITS} bits index")
    return [child | last << CHILD_BITS | letter << (CHILD_BITS + 1) | freq << (CHILD_BITS + 7)
            for child, last, letter, freq in nodes]


def c_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    words_dir, out_path = sys.argv[1:]
    out = ["/* Generated by gen-words.py from the word lists in words/: do not edit */", "",
           '#include "correct.h"', ""]
    dicts = []
    for name in sorted(os.listdir(words_dir)):
        if not name.endswith(".txt") or name.endswith(".fix.txt"):
            continue
        code = name[:-4]
        if code not in LETTERS:
            sys.exit(f"{name}: no letters known for the language {code} (gen-words.py)")
        letters = LETTERS[code]
        counts = read_list(os.path.join(words_dir, name), letters)
        fix = os.path.join(words_dir, f"{code}.fix.txt")
        if os.path.exists(fix):
            apply_fixes(fix, counts, letters)
        fold_plain(counts)
        if len(letters) > 1 << 6:
            sys.exit(f"{code}: more than 64 letters")
        nodes = build_trie(counts, letters)
        out.append(f"static const char *const {code}_letters[] = {{ "
                   + ", ".join(c_string(c) for c in letters) + " };")
        out.append(f"static const char {code}_base[] = {{ "
                   + ", ".join(f"'{base(c)}'" if base(c) else "0" for c in letters) + " };")
        out.append(f"static const uint32_t {code}_nodes[] = {{")
        for i in range(0, len(nodes), 8):
            out.append("\t" + ", ".join(f"0x{n:08x}" for n in nodes[i:i + 8]) + ",")
        out.append("};")
        out.append("")
        dicts.append(f"\t{{ {c_string(code)}, {code}_letters, {code}_base, {len(letters)}, "
                     f"{code}_nodes, {len(nodes)}, {max(map(frequency, counts.values()))} }},")
        print(f"{code}: {len(counts)} words, {len(nodes)} nodes, {len(nodes) * 4 // 1024} KB",
              file=sys.stderr)
    out.append("const struct correct_dict correct_dicts[] = {")
    out += dicts
    out.append("};")
    out.append(f"const int correct_dict_count = {len(dicts)};")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
