# Audiobooks library

The main menu's **Audiobooks** entry opens a library of the books in the
Audiobooks folder (`audiobook folder path`, `/Audiobooks` by default)
instead of a file browser. It has its own index and its own record of
progress; neither is the music database.

## Screens

- **Root:** a *Continue Listening* card for the last unfinished book
  (cover, title, author, progress bar, time left), then *Resume*,
  *Library*, *Authors*, *Series*, *Narrators*, *Recently Added*,
  *In Progress*, *Finished*, *Sleep Timer*, *Rescan Library* and
  *Audiobook Settings*.
- **Book lists:** three lines a book: the cover, the title (with the series
  number), the author, and how far through it is with the time left at the
  book's own speed.
- **Book page:** a card with the cover, author, narrator, length and
  progress, then *Resume*/*Play*, *Start Over*, *Chapters*, *Bookmarks*
  (when the folder has a `.bmark`), *Playback Speed*, *Mark as Finished*,
  *Sleep Timer* and *Description*.

## What counts as a book

- A folder with audio files in it. Folders named `CD 1`, `Disc 2`,
  `Part 3`, ... inside it are part of the same book, in natural order.
- A single `.m4b` file (or any audio file directly in the library root).

Title, author and the rest come from, in order:

1. The first file's tags: album = title, album artist / artist = author,
   composer = narrator (or a comment starting with "Read by" /
   "Narrated by"), grouping = series (`Name #3`, `Name, Book 3`).
2. The folder names: `Author/Series/NN - Title`, `Author/Title`, `Title`.
3. Sidecar files for what is still missing: `metadata.json`
   (Audiobookshelf / OpenAudible: `title`, `authors`, `narrators`,
   `series`, `description`), `reader.txt`, `desc.txt`.

The cover is `cover.jpg|jpeg|png` or `folder.jpg|jpeg|png` (for a single
file, a picture with the same name first), then any picture in the folder,
then the picture embedded in the first file.

## Chapters

The files of a folder book, or the chapters of an `.m4b` (embedded, or a
`.cue` next to it; the *mp4chapters_to_cue* plugin makes one).

## Files on the card

| File | What |
|---|---|
| `/.rockbox/audiobooks.db` | The index. Rebuilt by *Rescan Library*. |
| `/.rockbox/audiobooks.progress` | Where each book is up to, its speed and whether it is finished. Kept by book path, so a rescan keeps it. |

A rescan only reads the tags of books whose folder changed. With
*Scan at Startup* on (the default) that happens at boot, with a progress
screen only when there is something to read. *Rescan Library* reads
everything again, which is what to use after retagging files.

## While a book plays

- **Progress** is saved every minute, on pause and on stop.
- **Speed** set on the pitch screen while a book plays is remembered for
  that book and put back when it plays again. Music gets its own speed
  back when a song starts.
- **Finished:** past *Mark Finished At* (97 % by default), or when the
  last file plays out. *Resume* on a finished book starts it over.
- **Rewind after a pause:** a book paused for at least *Rewind After a
  Pause Of* (1 min) goes back *Rewind By* (60 s) when it resumes, and
  when it is started again from the library.
- **Sleep timer:** 15-120 minutes, or *End of Chapter*, which pauses when
  the current file, or the current cue chapter, ends.
- **Audiobook WPS:** Theme Settings and Audiobook Settings both have it.
  It picks the WPS used while a track from the Audiobooks folder plays;
  *Same as Music* keeps the normal one. Only the WPS changes, not the
  status bar.
- Prev/Next skip as before.
