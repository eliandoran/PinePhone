# Files

Files lets you look through the files on the phone: what is in a folder, how big a file is,
when it was changed, and the start of a text file. It only shows them: it cannot delete,
rename, copy or open files in other apps.

## Browsing

Open **Files** from the menu (under **Tools**). It starts on the screen titled **Files**, with
the folders where the apps keep what you put on the phone, the same ones a computer sees over
[USB file transfer](../computer.md#copying-files): **Photos**, **Music**,
**Books**, **Notes**, **Memos** (voice memos from [Recorder](recorder.md)), and **Genesis** and
**Doom WADs** once those apps are installed, each with its app's icon. After them,
:fontawesome-solid-list-ul: **Logs** holds the phone's logs, which help find out what went
wrong (see [Debugging](../debugging.md#what-to-look-at)); a log shows its start, the time it
began. Under them, :fontawesome-solid-inbox: **All files** opens the top of the phone's file
system.

![Where Files starts](../images/generated/files-places-light.webp#only-light){ width="300" }
![Where Files starts](../images/generated/files-places.webp#only-dark){ width="300" }

1. Tap a folder to open it. Its name becomes the title of the screen.
2. Tap the :fontawesome-solid-chevron-left: back arrow at the top left to go up one folder,
   and from the first folder back to where Files starts.
3. To see files that arrived while a folder is open (over USB, for example), pull the list
   down from its top and let go once the :fontawesome-solid-arrows-rotate: circle turns blue.

Folders come first, with a blue :fontawesome-solid-folder: folder symbol and an
:fontawesome-solid-chevron-right: arrow, then the files, each in alphabetical order. Each file
shows a symbol for its kind (:fontawesome-solid-image: picture, :fontawesome-solid-music:
sound, :fontawesome-solid-film: video, :fontawesome-solid-pen: text or :fontawesome-solid-file:
other), its size and when it was last changed: the time for today, the day of the week for the
past few days, otherwise the date.

![The photos folder](../images/generated/files-folder-light.webp#only-light){ width="300" }
![The photos folder](../images/generated/files-folder.webp#only-dark){ width="300" }

In **All files**, what the apps save is in **var › lib**, a folder each; the rest of the phone
is the system itself.

A folder with nothing in it says **Empty folder**; one you are not allowed to look into says
**Cannot open this folder**. A folder shows all its entries, however many: even the system's
largest open at once.

## A file's details

Tap a file. Its screen shows:

- **Size**, **Modified** (as in the list) and **Folder**, the folder it is in;
- for a text file, the start of it: up to 40 lines, or about 2,000 characters.

![A text file's details](../images/generated/files-text-light.webp#only-light){ width="300" }
![A text file's details](../images/generated/files-text.webp#only-dark){ width="300" }
