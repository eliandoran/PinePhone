# Books

Books reads ePub books, a chapter at a time, and remembers where you stopped in each one.

## Putting books on the phone

Books lists the ePub files (`.epub`) in the phone's books folder, **/var/lib/books**. Copy
them there from a computer. With no books yet, the app says **No books** and
**Copy ePub files to /var/lib/books**.

Books protected with DRM cannot be read: only DRM-free ePub files work, such as those from
Project Gutenberg or Standard Ebooks.

## The library

Open **Books** from the menu (under **Internet & media**).

![The library](../images/generated/books-library.webp){ width="300" }

Each book shows its title and author, in alphabetical order of title. A file that is not a
readable ePub shows its file name and **Cannot be read**, and does not open.

The app reads the folder each time you open it, so a new book shows the next time you
open Books.

## Reading

Tap a book to open it. It opens where you stopped last time, or at its start.

![A book open](../images/generated/books-reader.webp){ width="300" }

- **Tap the lower two thirds of the page** to go one screen on. At the end of a chapter, this
  goes to the start of the next one.
- **Tap the upper third** to go one screen back, into the end of the previous chapter from
  the start of one.
- **Drag** the page up or down to scroll it.
- **Tap a link** in the book (a table of contents inside the book, a note) to go there.
  Links to web pages do nothing.

The line under the page shows the chapter you are in and how far into the book you are, in
percent.

Your place is saved as you read, when you leave the book and when you change chapters.

## Contents

Tap the :fontawesome-solid-table-list: contents button at the top right for the book's
table of contents. The chapter you are in is in blue. Tap a chapter to go to its start.

![The contents](../images/generated/books-contents.webp){ width="300" }

A book without a table of contents shows **No contents**: page through it instead.

## Limitations

- The text is drawn with the phone's own fonts at a fixed size: the book's fonts are not used,
  and the text size cannot be changed yet.
- Pages are white, also in the dark theme.
- Going to another chapter takes a moment in books whose chapters are long.
- Images in SVG are not shown, except for covers made of a single picture.
