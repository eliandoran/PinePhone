#!/usr/bin/env python3
"""Tests of changelog.py: python3 scripts/test_changelog.py (just changelog-lint runs them)."""

import os
import sys
import unittest

sys.dont_write_bytecode = True  # no __pycache__ in scripts/
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import changelog  # noqa: E402

FILE = "20261110120000_slug.md"


def entry(frontmatter, text="Calls dropped after a minute."):
    parsed, errors = changelog.parse_entry(FILE, f"---\n{frontmatter}\n---\n\n{text}\n")
    assert parsed, errors
    return parsed


class Parse(unittest.TestCase):
    def test_reads_the_keys_and_the_text(self):
        e = entry("type: fix  # a comment\narea: Phone\ncloses: [12, '#13']\ncredits:\n  - '@someone'\n"
                  "notable: true", "Calls dropped.  \nOn 2G only.\n\n* Not on LTE\n")
        self.assertEqual((e.section, e.area, e.closes, e.credits, e.notable),
                         ("Bugfixes", "Phone", [12, 13], ["someone"], True))
        self.assertEqual(e.text, "Calls dropped.  ")
        self.assertEqual(e.details, ["On 2G only.", "", "* Not on LTE"])
        self.assertEqual(e.date, 1794312000)

    def test_refuses_what_it_does_not_know(self):
        _, errors = changelog.parse_entry(FILE, "---\ntype: bug\ncolour: red\ncloses: [x]\n---\n\n")
        self.assertEqual(len(errors), 4, errors)
        _, errors = changelog.parse_entry("notes.md", "---\ntype: fix\n---\n\nText\n")
        self.assertIn("YYYYMMDDHHMMSS", errors[0])


class Lint(unittest.TestCase):
    def test_a_fix_names_a_known_area(self):
        self.assertIn("required", changelog.lint_entry(entry("type: fix"))[0])
        self.assertIn("did you mean `Wi-Fi`", changelog.lint_entry(entry("type: fix\narea: wifi"))[0])
        self.assertEqual(changelog.lint_entry(entry("type: fix\narea: Wi-Fi")), [])
        self.assertEqual(changelog.lint_entry(entry("type: technical")), [])

    def test_links_are_full_urls(self):
        e = entry("type: improvement\narea: Notes", "See [Notes](../apps/notes.md).")
        self.assertIn("full URLs", changelog.lint_entry(e)[0])

    def test_a_highlight_has_a_title(self):
        self.assertIn("`title` is required", changelog.lint_entry(entry("type: highlight"))[0])

    def test_one_overview_and_for_a_release_one_at_least(self):
        overview = entry("type: overview")
        self.assertEqual(changelog.lint_overview([]), [])
        self.assertIn("no overview", changelog.lint_overview([], release=True)[0])
        self.assertIn("2 overview", changelog.lint_overview([overview, overview])[0])


class Render(unittest.TestCase):
    def test_sections_areas_and_notable_entries(self):
        entries = [
            entry("type: overview", "A release about calls."),
            entry("type: warning", "Back up /var/lib first."),
            entry("type: fix\narea: Phone", "Calls dropped."),
            entry("type: fix\narea: Phone\ncloses: [7]", "The speaker stayed on."),
            entry("type: fix\narea: Wi-Fi\nnotable: true", "Saved networks were forgotten."),
            entry("type: highlight\ntitle: Bluetooth calls", "Calls on a headset."),
        ]
        note = changelog.render_note("v0.2.0", entries)
        self.assertEqual(note, """# v0.2.0

> [!WARNING]
> Back up /var/lib first.

A release about calls.

Installing it, or updating to it: [Installing](https://eliandoran.github.io/PinePhone/install/).

## 💡 Key highlights

### Bluetooth calls

Calls on a headset.

## 🐞 Bugfixes

1.  Wi-Fi: Saved networks were forgotten.

<details>
    <summary>Phone (2)</summary>
    <ol>
        <li>Calls dropped.</li>
        <li><a href="https://github.com/eliandoran/PinePhone/issues/7">The speaker stayed on.</a></li>
    </ol>
</details>
""")

    def test_unreleased_has_no_preamble(self):
        note = changelog.render_note("Unreleased changes", [entry("type: technical", "Linux 7.1.")],
                                     preamble=False)
        self.assertEqual(note, "# Unreleased changes\n\n## 🛠️ Technical updates\n\n1.  Linux 7.1.\n")

    def test_markdown_to_html(self):
        self.assertEqual(changelog.markdown_to_html("`a<b>` **c** *d*", ["* one", "* two", "more"]),
                         "<code>a&lt;b&gt;</code> <strong>c</strong> <em>d</em>"
                         "<ul><li>one</li><li>two</li></ul> more")

    def test_autolinks_become_links(self):
        self.assertEqual(changelog.markdown_to_html("See <https://a.org/x#y>."),
                         'See <a href="https://a.org/x#y">https://a.org/x#y</a>.')


class Unreleased(unittest.TestCase):
    def test_a_nightly_says_what_it_was_built_from_and_since_when(self):
        note = changelog.unreleased_note([entry("type: fix\narea: Phone", "Calls dropped.")],
                                         "v0.1.0", "abc1234", "2026-11-11")
        self.assertEqual(note.split("\n## ")[0], """# Changes since v0.1.0

> [!NOTE]
> The nightly build of [`abc1234`](https://github.com/eliandoran/PinePhone/commit/abc1234) (2026-11-11), not a version: the changes since [v0.1.0](https://github.com/eliandoran/PinePhone/releases/tag/v0.1.0), as the next version's note will list them. Tried less than a version.
""")

    def test_before_the_first_version(self):
        note = changelog.unreleased_note([], None, "abc1234", "2026-11-11")
        self.assertTrue(note.startswith("# Unreleased changes\n"))
        self.assertIn("not a version: the changes so far.", note)


class Nav(unittest.TestCase):
    def test_adds_releases_then_newest_first(self):
        source = 'nav = [\n  { "Home" = "index.md" },\n]\n'
        once = changelog.add_to_nav("v0.2.0", source)
        twice = changelog.add_to_nav("v0.3.0", once)
        self.assertEqual(twice, 'nav = [\n  { "Home" = "index.md" },\n  { "Releases" = [\n'
                                '    { "v0.3.0" = "releases/v0.3.0.md" },\n'
                                '    { "v0.2.0" = "releases/v0.2.0.md" },\n  ] },\n]\n')
        self.assertEqual(changelog.add_to_nav("v0.3.0", twice), twice)


if __name__ == "__main__":
    unittest.main()
