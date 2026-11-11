# Listening history

Every listen is appended to `/.rockbox/listens.tsv` on the card, one line each, when the track ends for any reason. It is the record the recaps, charts and recommendations of #28 will be built on; the first reader is the radio's *Station History*.

| Column | Meaning |
| --- | --- |
| `path` | The file |
| `started` | When it started, Unix time |
| `played_ms` | How much of it played |
| `length_ms` | Its length at the time, so `played_ms / length_ms` is how much was heard |
| `ended` | `finished` when playback moved on by itself, `skipped` when it was skipped or stopped |
| `source` | `music`, `audiobook`, or `radio` for a pseudo radio station |
| `station` | The station's name when the source is `radio`, else empty. Rows written before this column existed end at `source` |

A track that never started is not logged. A short listen is, and marked by its `played_ms`, so skips are data too.

The file is tab separated, so a spreadsheet opens it as it is. Past 8 MiB (about 70 000 listens) it moves to `listens.1.tsv` and a new one starts. It is not part of the database: rebuilding the database keeps it, and a listen to a file that has since moved still counts.

*Settings > Library > Advanced > Listening History* turns it off. Delete the file to forget it. *Gather Runtime Data* (play counts and last played, in the database) is now on by default too.
