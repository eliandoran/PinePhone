# Word lists for the keyboard's autocorrection

`<code>.txt` is a language's words with how often each occurs, one per line (`word count`),
from the OpenSubtitles 2018 frequency lists of
[hermitdave/FrequencyWords](https://github.com/hermitdave/FrequencyWords)
(`content/2018/<code>/<code>_50k.txt`, MIT license, Copyright (c) 2016 Hermit Dave), as
downloaded: `gen-words.py` turns them into `words.c` (`correct.c`'s dictionaries) in each
build, keeping only words written with the language's letters (plus an apostrophe or a
hyphen inside a word), cedilla ş/ţ as comma-below ș/ț, and a word's spelling without its
diacritics folded into it when it is no more frequent (subtitles often lack them: `stii`
is `știi` mistyped, `fata` stays a word of its own beside `fată`).

`<code>.fix.txt` corrects a list by hand: `old -> new` renames a word (counts added up),
`word count` sets a count (0 removes the word). English needs it because the lists
tokenized contractions apart (`don` 4.1M and `'t` 9.6M, no `don't`): stems that are only
contractions are renamed, and the pronoun forms (`i'm`, `it's`, `you're`...) get counts
estimated from their endings' totals, in the right order of magnitude rather than measured.
