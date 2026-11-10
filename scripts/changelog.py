#!/usr/bin/env python3
"""Release notes from changelog entries: a Markdown file in .changelog/ per change a user can
notice, collected when a version is released into docs/releases/<version>.md, which is both a page
of the documentation (Releases, in zensical.toml's nav) and the GitHub release's text (image.yml).
Carried over from Trilium Notes' scripts/release-notes.mts, without what reads GitHub there
(closed issues and their contributors, nightly builds, new languages, protocol versions).

    changelog.py new <slug>          writes .changelog/<YYYYMMDDHHMMSS>_<slug>.md to fill in
    changelog.py lint [--release]    checks every entry; --release: also that there is an overview
    changelog.py <version> [--stdout] [--force]
                                     the release note, into docs/releases/<version>.md (v0.2.0)
    changelog.py unreleased          the note of the entries so far, printed: the nightly
                                     release's text (image.yml), with a notice of the commit
    changelog.py consume <version>   in the release commit, once the note is final: deletes the
                                     entries, or moves them to .changelog/pre-release/ for a
                                     pre-release (v0.2.0-rc.1), so the release lists them again

The format: .changelog/README.md.
"""

import argparse
import os
import re
import subprocess
import sys
from dataclasses import dataclass, field
from datetime import datetime, timezone

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CHANGELOG_DIR = os.path.join(ROOT, ".changelog")
PRE_RELEASE_DIR = os.path.join(CHANGELOG_DIR, "pre-release")
RELEASES_DIR = os.path.join(ROOT, "docs", "releases")
ZENSICAL = os.path.join(ROOT, "zensical.toml")
REPOSITORY = os.environ.get("GITHUB_REPOSITORY") or "eliandoran/PinePhone"
DOCS_URL = "https://eliandoran.github.io/PinePhone/"
ENTRY_FILE_NAME = re.compile(r"^(\d{14})_[\w.-]+\.md$")
VERSION = re.compile(r"^v\d+\.\d+\.\d+(-[\w.]+)?$")

# The changelog entry `type` values, each naming the release note section it goes to, in the
# note's order
ENTRY_TYPES = {
    "note": "Note",
    "tip": "Tip",
    "important": "Important",
    "warning": "Warning",
    "overview": "Overview",
    "highlight": "Key highlights",
    "breaking": "Breaking changes",
    "fix": "Bugfixes",
    "improvement": "Improvements",
    "i18n": "Languages",
    "technical": "Technical updates",
    "security": "Security improvements",
}
HEADINGS = {
    "Key highlights": "💡",
    "Breaking changes": "🚨",
    "Bugfixes": "🐞",
    "Improvements": "✨",
    "Languages": "🌍",
    "Technical updates": "🛠️",
    "Security improvements": "🔒️",
}
# Sections written as GitHub alerts above the first heading instead of as a list
ADMONITIONS = {"Note", "Tip", "Important", "Warning"}
# Sections that fold each area into a <details> block; `notable` keeps an entry out of it
COLLAPSIBLE = set(HEADINGS) - {"Key highlights"}
# Sections listed with bullets; the others are numbered. Key highlights are headings instead.
BULLETED = {"Breaking changes"}
# Sections whose entries must name an area
AREA_SECTIONS = {"Bugfixes", "Improvements"}
KEYS = ("type", "title", "area", "closes", "credits", "notable")

# The areas an entry can name, each a collapsible group of its section. A fix or an improvement
# must name one, so Bugfixes and Improvements list no loose items; a change that fits none adds
# its area here.
AREAS = [
    # Built-in apps
    "Phone", "Messages", "Contacts", "Camera", "Gallery", "Music", "Videos", "Clock", "Calendar",
    "Calculator", "Notes", "Tasks", "Recorder", "Files", "Maps", "Weather", "Compass", "Web Lite",
    "Books", "Snake", "Settings",
    # External apps
    "External apps", "Telegram", "GitHub", "Email", "Assistant", "Browser", "Terminal", "Doom",
    "Genesis", "Trilium",
    # The rest of the phone
    "General", "Home screen", "Lock screen", "Notifications", "Status bar", "Keyboard", "Display",
    "Sound", "Battery", "Sleep and wake", "Performance", "Mobile network", "Mobile data", "Wi-Fi",
    "Bluetooth", "USB", "Sensors", "Installing and updating", "Boot",
]


@dataclass
class Entry:
    """A file in .changelog/, describing one change the way the release note lists it."""
    file: str
    date: int  # the time in the file name, in seconds since the epoch
    section: str
    area: str | None = None
    closes: list[int] = field(default_factory=list)
    credits: list[str] = field(default_factory=list)
    notable: bool = False  # listed above its section's collapsible blocks, not in its area's
    title: str | None = None  # the ### heading of a Key highlights entry
    text: str = ""  # the first line, which becomes the list item
    details: list[str] = field(default_factory=list)  # the lines after it, nested under it


def parse_entry(file, source):
    """Parses an entry: frontmatter in a small subset of YAML (`key: value`, `key: [a, b]` or a
    `- item` list, `#` comments), then the text. Returns (entry, []) or (None, its problems)."""
    errors = []
    match = re.match(r"^---\r?\n(.*?)\r?\n---\r?\n(.*)$", source, re.S)
    timestamp = ENTRY_FILE_NAME.match(os.path.basename(file))
    if not match or not timestamp:
        return None, ["expected a `YYYYMMDDHHMMSS_<slug>.md` file starting with `---` frontmatter"]

    fields = {}
    current = None
    for raw in match.group(1).splitlines():
        line = re.sub(r"\s+#\s.*$", "", raw).rstrip()
        if not line.strip() or line.strip().startswith("#"):
            continue
        item = re.match(r"^\s+-\s*(.*)$", line)
        if item:
            if current is None:
                errors.append(f"list item outside a key: {line.strip()}")
            elif item.group(1):
                current.append(unquote(item.group(1)))
            continue
        pair = re.match(r"^([A-Za-z]+):\s*(.*)$", line)
        if not pair:
            errors.append(f"unreadable line: {line.strip()}")
            continue
        key, value = pair.groups()
        inline = re.match(r"^\[(.*)\]$", value)
        if inline:
            current = [unquote(it.strip()) for it in inline.group(1).split(",") if it.strip()]
        else:
            current = [unquote(value)] if value else []
        fields[key] = current

    for key in fields:
        if key not in KEYS:
            errors.append(f"unknown key `{key}`; expected {', '.join(KEYS)}")

    kind = (fields.get("type") or [None])[0]
    section = ENTRY_TYPES.get(kind)
    if not section:
        found = f", not `{kind}`" if kind else ""
        errors.append(f"`type` must be one of {', '.join(ENTRY_TYPES)}{found}")

    closes = []
    issue_url = re.compile(r"^https://github\.com/[\w.-]+/[\w.-]+/(?:issues|pull)/(\d+)$")
    for reference in fields.get("closes", []):
        number = re.match(r"^#?(\d+)$", reference) or issue_url.match(reference)
        if number:
            closes.append(int(number.group(1)))
        else:
            errors.append(f"`closes` takes issue numbers or URLs, not `{reference}`")

    credits = []
    for credit in fields.get("credits", []):
        login = credit.removeprefix("@")
        if re.match(r"^[A-Za-z0-9](?:[A-Za-z0-9-]*[A-Za-z0-9])?$", login):
            credits.append(login)
        else:
            errors.append(f"`credits` takes GitHub usernames, not `{credit}`")

    notable = (fields.get("notable") or ["false"])[0]
    if notable not in ("true", "false"):
        errors.append(f"`notable` is true or false, not `{notable}`")

    text, *details = match.group(2).strip().splitlines() or [""]
    if not text:
        errors.append("the entry has no text after the frontmatter")

    if errors:
        return None, errors
    date = int(datetime.strptime(timestamp.group(1), "%Y%m%d%H%M%S")
               .replace(tzinfo=timezone.utc).timestamp())
    return Entry(file=file, date=date, section=section, area=(fields.get("area") or [None])[0],
                 closes=closes, credits=credits, notable=notable == "true",
                 title=(fields.get("title") or [None])[0], text=trim_keeping_break(text.lstrip()),
                 details=trim_blank_lines([trim_keeping_break(it) for it in details])), []


def lint_entry(entry):
    """The problems of a well-formed entry: a key that has no effect where it is, a fix or an
    improvement without an area or with one missing from AREAS, a relative link."""
    problems = []
    if any(re.search(r"\]\((?!https?://)", line) for line in [entry.text, *entry.details]):
        problems.append("links take full URLs (the documentation is under "
                        f"{DOCS_URL}): the note is also the GitHub release's text")
    if entry.notable and entry.section not in COLLAPSIBLE:
        problems.append(f"`notable` has no effect in {entry.section}, which lists every entry")
    if entry.section == "Key highlights" and not entry.title:
        problems.append("`title` is required for a highlight: the heading its paragraphs go under")
    if entry.title and entry.section != "Key highlights":
        problems.append(f"`title` has no effect in {entry.section}, which lists every entry")
    if entry.area and entry.section == "Overview":
        problems.append("`area` has no effect in the overview, which is written above the sections")

    known = ", ".join(AREAS)
    if not entry.area:
        if entry.section in AREA_SECTIONS:
            problems.append(f"`area` is required for a fix or an improvement; one of {known}")
    elif entry.area not in AREAS:
        wanted = entry.area.lower()
        closest = next((area for area in AREAS if area.lower() == wanted), None) \
            or next((area for area in AREAS if edit_distance(area.lower(), wanted) <= 2), None)
        problems.append(f"unknown area `{entry.area}`; did you mean `{closest}`?" if closest else
                        f"unknown area `{entry.area}`; one of {known}, or add it to `AREAS` in "
                        "scripts/changelog.py")
    return problems


def lint_overview(entries, release=False):
    """Several overviews are an error at any time; none, when `release` is set."""
    overviews = [entry for entry in entries if entry.section == "Overview"]
    if len(overviews) > 1:
        return [f"{len(overviews)} overview entries; a release has one: "
                + ", ".join(os.path.basename(entry.file) for entry in overviews)]
    if not overviews and release:
        return ["no overview entry; a release opens with one (`type: overview`, see "
                ".changelog/README.md)"]
    return []


def edit_distance(a, b):
    previous = list(range(len(b) + 1))
    for i, char_a in enumerate(a):
        current = [i + 1]
        for j, char_b in enumerate(b):
            current.append(min(previous[j + 1] + 1, current[j] + 1,
                               previous[j] + (char_a != char_b)))
        previous = current
    return previous[-1]


def entry_files():
    """The .md files of .changelog/pre-release/ and .changelog/, but README.md, oldest first."""
    for directory in (PRE_RELEASE_DIR, CHANGELOG_DIR):
        if os.path.isdir(directory):
            for name in sorted(os.listdir(directory)):
                if name.endswith(".md") and name != "README.md":
                    yield os.path.join(directory, name)


def read_entries():
    """The entries that parse, and the problems of the others."""
    entries, invalid = [], []
    for file in entry_files():
        with open(file, encoding="utf-8") as f:
            entry, errors = parse_entry(file, f.read())
        if entry:
            entries.append(entry)
        else:
            invalid.append((file, errors))
    return entries, invalid


def linked_text(entry):
    """The entry's text linked to the issues it closes: the text itself for one issue (unless it
    has a link of its own), `(#N, #M)` after it otherwise. Not a highlight's."""
    text = entry.text
    if entry.section == "Key highlights":
        return text
    links = [(n, f"https://github.com/{REPOSITORY}/issues/{n}") for n in entry.closes]
    unlinked = [(n, url) for n, url in links if not re.search(rf"/(?:issues|pull)/{n}(?!\d)", text)]
    if len(links) == 1 and "](" not in text:
        return f"[{text}]({links[0][1]})"
    if unlinked:
        return f"{text} ({', '.join(f'[#{n}]({url})' for n, url in unlinked)})"
    return text


def render_note(title, entries, preamble=True):
    """The release note: its alerts, the overview, how to install it (`preamble`), then a
    section per type with entries, oldest first in each."""
    entries = sorted(entries, key=lambda entry: entry.date)
    lines = [f"# {title}", ""]
    for entry in entries:
        if entry.section in ADMONITIONS:
            first, *rest = render_item("", linked_text(entry), entry, "")
            lines += [f"> [!{entry.section.upper()}]", f"> {first}",
                      *(f"> {line}" if line else ">" for line in rest), ""]
    for entry in entries:
        if entry.section == "Overview":
            lines += [entry.text, *([""] if entry.details else []), *entry.details, ""]
    if preamble:
        lines += [f"Installing it, or updating to it: [Installing]({DOCS_URL}install/).", ""]

    for section, emoji in HEADINGS.items():
        items = [entry for entry in entries if entry.section == section]
        if items:
            lines += [f"## {emoji} {section}", ""]
            lines += (render_highlights(items) if section == "Key highlights" else
                      render_section(items, bulleted=section in BULLETED,
                                     collapsible=section in COLLAPSIBLE))
            lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def render_highlights(items):
    """Each Key highlights item as a ### heading over its text and detail lines as written;
    the credits close it as a line of their own."""
    lines = []
    for item in items:
        credit = ["", f"Contributed by {' and '.join('@' + login for login in item.credits)}."] \
            if item.credits else []
        lines += [*([""] if lines else []), f"### {item.title}", "", item.text,
                  *([""] if item.details else []), *item.details, *credit]
    return lines


def render_section(items, bulleted=False, collapsible=False):
    """Lists a section's items, prefixing a lone item with its area. The items of an area that
    has several go under one item named after it. When `collapsible`, every area goes into a
    <details> block after the list instead, written in HTML with the number of items in its
    summary, and only the items without an area stay in the list, after the notable ones, which
    the list keeps out of their area's block."""
    groups = []
    for item in items:
        notable = collapsible and item.notable
        group = next((g for g in groups if g["area"] == item.area and not g["notable"]), None) \
            if item.area and not notable else None
        if group:
            group["items"].append(item)
        else:
            groups.append({"area": item.area, "items": [item], "notable": notable})

    collapsed = [g for g in groups if g["area"] and not g["notable"]] if collapsible else []
    listed = sorted((g for g in groups if g not in collapsed), key=lambda g: not g["notable"])

    def marker(index):
        return "*   " if bulleted else f"{index + 1}.  "

    lines = []
    for index, group in enumerate(listed):
        area = group["area"]
        if len(group["items"]) == 1:
            item = group["items"][0]
            text = linked_text(item)
            named = area and text.lstrip("[").lower().startswith(area.lower())
            lines += render_item(marker(index), f"{area}: {text}" if area and not named else text,
                                 item, " " * 4)
            continue
        lines.append(f"{marker(index)}{area}")
        for position, item in enumerate(group["items"]):
            lines += render_item(f"    {marker(position)}", linked_text(item), item, " " * 8)

    tag = "ul" if bulleted else "ol"
    for group in collapsed:
        lines += [*([""] if lines else []), "<details>",
                  f"    <summary>{escape_html(group['area'])} ({len(group['items'])})</summary>",
                  f"    <{tag}>"]
        for item in group["items"]:
            text, *details = render_item("", linked_text(item), item, "")
            lines.append(f"        <li>{markdown_to_html(text, details)}</li>")
        lines += [f"    </{tag}>", "</details>"]
    return lines


def render_item(marker, text, item, indent):
    credit = f" by {' & '.join('@' + login for login in item.credits)}" if item.credits else ""
    return [f"{marker}{text}{credit}", *(indent + line if line else "" for line in item.details)]


def markdown_to_html(text, details=()):
    """The HTML of one list item from an entry's Markdown: its text and its detail lines, whose
    `*` or `1.` items become a nested list and whose other lines continue the text. Only the
    inline syntax entries use: links and autolinks, `code`, **strong**, *emphasis*, backslash
    escapes and two-space hard breaks."""
    out = inline_to_html(text)
    lst = None

    def close_list():
        nonlocal out, lst
        if lst:
            out += f"<{lst[0]}>" + "".join(f"<li>{it}</li>" for it in lst[1]) + f"</{lst[0]}>"
            lst = None

    hard_break = text.endswith("  ")
    for line in details:
        item = re.match(r"^\s*(\*|\d+\.)\s+(.*)$", line)
        if item:
            tag = "ul" if item.group(1) == "*" else "ol"
            if not lst or lst[0] != tag:
                close_list()
                lst = (tag, [])
            lst[1].append(inline_to_html(item.group(2)))
        elif line.strip():
            close_list()
            out += ("<br>" if hard_break else " ") + inline_to_html(line.strip())
        hard_break = line.endswith("  ")
    close_list()
    return out


def inline_to_html(text):
    kept = []

    def keep(fragment):
        kept.append(fragment)
        return f"\0{len(kept) - 1}\0"

    out = re.sub(r"`([^`]+)`", lambda m: keep(f"<code>{escape_html(m.group(1))}</code>"),
                 text.rstrip())
    out = re.sub(r"\\([\\`*_\[\]()#+\-.!<>])", lambda m: keep(escape_html(m.group(1))), out)
    out = re.sub(r"&(?:[a-z]+|#\d+);", lambda m: keep(m.group(0)), out, flags=re.I)
    out = re.sub(r"<(https?://[^>\s]+)>",
                 lambda m: keep(f'<a href="{escape_html(m.group(1))}">{escape_html(m.group(1))}</a>'),
                 out)
    out = escape_html(out)
    out = re.sub(r"\[([^\]]*)\]\(([^)\s]+)\)", r'<a href="\2">\1</a>', out)
    out = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", out)
    out = re.sub(r"(^|[^*\w])\*([^*\s][^*]*?)\*(?!\w)", r"\1<em>\2</em>", out)
    out = re.sub(r"(^|[^\w])_([^_\s][^_]*?)_(?!\w)", r"\1<em>\2</em>", out)
    return re.sub(r"\0(\d+)\0", lambda m: kept[int(m.group(1))], out)


def escape_html(text):
    return (text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def trim_keeping_break(line):
    """Trims the end of a line but keeps a Markdown hard break (two trailing spaces)."""
    stripped = line.rstrip()
    return stripped + "  " if stripped and re.fullmatch(r" {2,}", line[len(stripped):]) else stripped


def unquote(value):
    return value[1:-1] if re.match(r"""^(["']).*\1$""", value) else value


def trim_blank_lines(lines):
    while lines and not lines[0]:
        lines = lines[1:]
    while lines and not lines[-1]:
        lines = lines[:-1]
    return lines


def add_to_nav(version, nav_source):
    """zensical.toml with the version's note first under Releases, a section added at the end
    of the navigation when there is none yet."""
    line = f'    {{ "{version}" = "releases/{version}.md" }},'
    if f'"releases/{version}.md"' in nav_source:
        return nav_source
    lines = nav_source.split("\n")
    releases = next((i for i, it in enumerate(lines) if it.strip() == '{ "Releases" = ['), None)
    if releases is not None:
        lines.insert(releases + 1, line)
    else:
        start = lines.index("nav = [")
        end = next(i for i in range(start, len(lines)) if lines[i] == "]")
        lines[end:end] = ['  { "Releases" = [', line, "  ] },"]
    return "\n".join(lines)


def unreleased_note(entries, base, commit, date):
    """The note of the entries so far, under a notice naming the nightly build's commit, its
    date and the version they follow (`base`, None before the first)."""
    since = (f"the changes since [{base}](https://github.com/{REPOSITORY}/releases/tag/{base}), "
             "as the next version's note will list them" if base else "the changes so far")
    notice = Entry(file="", date=0, section="Note",
                   text=f"The nightly build of [`{commit}`](https://github.com/{REPOSITORY}/commit/"
                        f"{commit}) ({date}), not a version: {since}. Tried less than a version.")
    return render_note(f"Changes since {base}" if base else "Unreleased changes",
                       [notice, *entries], preamble=False)


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def lint(release=False):
    problems, entries = [], []
    for file in entry_files():
        with open(file, encoding="utf-8") as f:
            entry, errors = parse_entry(file, f.read())
        relative = os.path.relpath(file, ROOT)
        problems += [(relative, message) for message in errors + (lint_entry(entry) if entry else [])]
        if entry:
            entries.append(entry)
    problems += [(".changelog", message) for message in lint_overview(entries, release)]
    for file, message in problems:
        print(f"::error file={file}::{message}" if os.environ.get("GITHUB_ACTIONS")
              else f"{file}: {message}", file=sys.stderr)
    if problems:
        sys.exit(f"{len(problems)} problems in .changelog/; see .changelog/README.md.")
    print("Every changelog entry is well defined.")


def release_note(version, stdout=False, force=False):
    path = os.path.join(RELEASES_DIR, f"{version}.md")
    if not stdout and not force and os.path.exists(path):
        sys.exit(f"{path} already exists; pass --force to overwrite it or --stdout to print it.")
    entries, invalid = read_entries()
    missing_overview = lint_overview(entries, release=True)
    if missing_overview and not stdout:
        sys.exit(missing_overview[0])

    note = render_note(version, entries)
    if stdout:
        sys.stdout.write(note)
    else:
        os.makedirs(RELEASES_DIR, exist_ok=True)
        with open(path, "w", encoding="utf-8") as f:
            f.write(note)
        with open(ZENSICAL, encoding="utf-8") as f:
            nav = f.read()
        with open(ZENSICAL, "w", encoding="utf-8") as f:
            f.write(add_to_nav(version, nav))
        print(f"Wrote {os.path.relpath(path, ROOT)} from {len(entries)} changelog entries.",
              file=sys.stderr)
    loose = [entry for entry in entries if not entry.area and entry.section in COLLAPSIBLE
             and any(other.section == entry.section and other.area for other in entries)]
    if loose:
        print(f"  {len(loose)} entries have no area, so they stay outside the groups:",
              file=sys.stderr)
        for entry in loose:
            print(f"    {entry.section}: {entry.text[:80]}", file=sys.stderr)
    for file, errors in invalid:
        print(f"  left out {os.path.relpath(file, ROOT)}:\n    " + "\n    ".join(errors),
              file=sys.stderr)
    for message in missing_overview:
        print(f"  {message}", file=sys.stderr)


def consume(version):
    """Deletes the entries a release note has listed, or keeps them in pre-release/ for a
    pre-release. Invalid entries stay where they are, so the next run reports them again."""
    entries, invalid = read_entries()
    if invalid:
        print(f"Keeping {len(invalid)} invalid entries: "
              + ", ".join(os.path.relpath(file, ROOT) for file, _ in invalid), file=sys.stderr)
    pre_release = "-" in version
    for entry in entries:
        if not pre_release:
            os.unlink(entry.file)
        elif os.path.dirname(entry.file) != PRE_RELEASE_DIR:
            os.makedirs(PRE_RELEASE_DIR, exist_ok=True)
            os.rename(entry.file, os.path.join(PRE_RELEASE_DIR, os.path.basename(entry.file)))
    if os.path.isdir(PRE_RELEASE_DIR) and not os.listdir(PRE_RELEASE_DIR):
        os.rmdir(PRE_RELEASE_DIR)
    print(f"{'Moved' if pre_release else 'Deleted'} {len(entries)} changelog entries.",
          file=sys.stderr)


def wrap(items, width):
    """Joins the items with commas into lines of at most `width` characters."""
    lines = []
    for index, item in enumerate(items):
        word = f"{item}," if index < len(items) - 1 else item
        if lines and len(lines[-1]) + len(word) < width:
            lines[-1] += f" {word}"
        else:
            lines.append(word)
    return lines


def new_entry(slug):
    """Writes an entry to fill in, named after the current time and the slug."""
    if not re.match(r"^[\w.-]+$", slug):
        sys.exit("Usage: just changelog-new <slug>, with a slug of letters, digits, `-`, `_` or `.`.")
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%d%H%M%S")
    path = os.path.join(CHANGELOG_DIR, f"{timestamp}_{slug}.md")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join([
            "---",
            f"# {' | '.join(ENTRY_TYPES)}",
            "type: fix",
            "# Required for highlight: the heading its paragraphs go under.",
            "# title:",
            "# Required for fix and improvement: the group the entry goes to, one of",
            *(f"# {line}" for line in wrap(AREAS, 96)),
            "area:",
            "# Optional: issue numbers, which the entry links to.",
            "closes: []",
            "# Optional: GitHub usernames of outside contributors whose code is the change.",
            "credits: []",
            "# Optional: true lists the entry above the collapsible blocks. For lost data, a",
            "# phone that no longer calls, texts or updates, a regression, a changed default;",
            "# never for a new feature, which competes for a highlight instead.",
            "notable: false",
            "---",
            "",
            "What changed, the way a user notices it.",
            "",
        ]))
    print(os.path.relpath(path, ROOT))


def main():
    parser = argparse.ArgumentParser(description="Release notes from .changelog/'s entries.")
    parser.add_argument("command", help="new, lint, unreleased, consume or a version (v0.2.0)")
    parser.add_argument("argument", nargs="?", help="new's slug, consume's version")
    parser.add_argument("--release", action="store_true", help="lint: an overview is required")
    parser.add_argument("--stdout", action="store_true", help="print the note, write nothing")
    parser.add_argument("--force", action="store_true", help="overwrite an existing note")
    args = parser.parse_args()

    if args.command == "new" and args.argument:
        new_entry(args.argument)
    elif args.command == "lint":
        lint(args.release)
    elif args.command == "unreleased":
        entries, _ = read_entries()
        commit, date = git("log", "-1", "--format=%h %cs").split()
        base = git("describe", "--tags", "--abbrev=0", "--match", "v[0-9]*") or None
        sys.stdout.write(unreleased_note(entries, base, commit, date))
    elif args.command == "consume" and args.argument and VERSION.match(args.argument):
        consume(args.argument)
    elif VERSION.match(args.command) and not args.argument:
        release_note(args.command, args.stdout, args.force)
    else:
        parser.print_usage(sys.stderr)
        sys.exit("expected: new <slug> | lint [--release] | <version> [--stdout] [--force] | "
                 "unreleased | consume <version>")


if __name__ == "__main__":
    main()
