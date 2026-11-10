# Changelog entries

Each file here describes one change the way the next release note lists it. At release time,
`just release-notes <version>` collects them into `docs/releases/<version>.md`: a page of the
documentation (listed under Releases in `zensical.toml`, which it updates) and the text of the
GitHub release that the tag's image build publishes (`image.yml`). `just release-notes consume
<version>` deletes them in the release commit, once the note is final. For a pre-release
(`v0.2.0-rc.1`) it moves them to `pre-release/` instead, so the final release lists them again.

`just release-notes unreleased` prints the note of the entries so far.

## When to add one

Add an entry for every change a user can notice: a fixed bug, a new or changed app, screen,
setting or gesture, a changed default, new flashing or installing steps, a breaking change (a
plugin app to rebuild, data that does not carry over), a security fix. One change can need several
entries (a branch that fixes three unrelated things), and several commits can share one (a feature
built over twenty commits).

Skip the entry when:

- the change only fixes something introduced since the last release;
- the change is internal: tests, the simulator, the browser demo, tooling, CI, notes,
  refactoring, documentation alone.

## The overview

One entry per release has `type: overview` and nothing else in its frontmatter. Its text goes
above the first section, after the notices, as a few paragraphs: what kind of release this is,
which themes the highlights below belong to, and what to know before updating. It is the one part
of the note that is written rather than collected, so write it last, once the entries are in.

A release requires it: `just changelog-lint --release` and `just release-notes <version>` fail
without one, while the plain lint and `unreleased` do not. Two overview entries are an error
everywhere. `consume` removes it with the other entries, and moves it to `pre-release/` for a
pre-release, so the final release reuses it.

## Creating one

```sh
just changelog-new <slug>
```

This writes `.changelog/<YYYYMMDDHHMMSS>_<slug>.md` from a template. The timestamp orders the
entries in the note and keeps two branches from writing the same file.

`just changelog-lint` checks every entry: it must parse, and a fix or an improvement must name a
known area. The Changelog workflow runs it on each pull request and push to `main` that touches
`.changelog/`.

## Format

```markdown
---
type: fix
area: Messages
closes: [42]
---

A long message sent while on 2G arrived in pieces out of order.
```

| Key | Required | Value |
|---|---|---|
| `type` | yes | The release note section: `overview` (above), `highlight`, `breaking`, `fix`, `improvement`, `i18n` (a new language, or one completed), `technical` (the kernel, Buildroot, the bootloader...) or `security`. `note`, `tip`, `important` and `warning` become a GitHub alert above the first section instead, for a notice that concerns the whole release (“Update the plugin apps after this one”). |
| `title` | for `highlight` | The `###` heading of a Key highlights entry. The text below the frontmatter is written under it as it stands: paragraphs separated by blank lines, `*` lists, links. Write it as a short story: what a user could not do, what they can do now, where to find it. |
| `area` | for `fix` and `improvement` | Puts the entry in a collapsible block named after it, such as `Phone`, `Wi-Fi` or `Installing and updating`, even when it is the only entry of that area. One of `AREAS` in `scripts/changelog.py`, which the template lists; a change that fits none adds its area there. |
| `closes` | no | Issue or pull request numbers or URLs. The note links the entry to them. It does not close the issue: that is still the `Closes #N` of the commit or pull request. |
| `credits` | no | GitHub usernames to thank: an outside contributor whose code is the change, not the reporter of the bug or the author of the idea. Leave out the maintainer. |
| `notable` | no | `true` lists the entry above the section's collapsible blocks, prefixed with its area, so a reader who opens none still sees it. For a change the reader is worse off missing: lost data, a phone that no longer called, texted, woke or updated, a regression since the previous release, a changed default. Never for a new feature, however large: that competes for Key highlights instead. |

After the frontmatter, the first line is the entry (for a highlight, its first paragraph). Describe
what the user sees, not the code: "An incoming call while asleep rang only after the third
ring", not "Ask for AT+CLCC on resume". Past tense for a fix. Lines after the first are nested
under the entry, for details or sub-items.

Links take full URLs: the note is also the GitHub release's text, where a link relative to the
documentation leads nowhere. The documentation is at <https://eliandoran.github.io/PinePhone/>
(`docs/apps/notes.md` is `https://eliandoran.github.io/PinePhone/apps/notes/`).

The frontmatter is read by a small parser, not a full YAML one: keep to `key: value`,
`key: [a, b]` or a `- item` list per key, with `#` comments.

## Releasing

1. Write the overview entry (`type: overview`).
2. `just release-notes v0.2.0` writes `docs/releases/v0.2.0.md` and lists it in `zensical.toml`;
   read it, fix the entries rather than the note while they are there, and run it again with
   `--force`. Edit the note itself only for what the entries cannot say.
3. `just release-notes consume v0.2.0`, then commit the note, `zensical.toml` and the removed
   entries together.
4. Tag that commit `v0.2.0` and push the tag: the image build publishes the release with the note
   as its text (without its `#` title, which the release has).
