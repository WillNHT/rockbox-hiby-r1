# Library Views and Playlist Covers

## Library View

**Theme Settings > Library View** changes how albums, tracks, playlists and audiobooks are listed:

| View | Layout |
|---|---|
| Classic | The plain text list |
| Thumbnails | One line per item, with a small cover |
| Shelves (default) | One large card per item: cover, name, and a second line |
| Grid | Two covers per row |
| Carousel | The selected cover large in the middle, with the previous and next covers above and below it |

The stick works the same in every view: up and down move through the items, and a sideways flick means yes or no. In Grid, up and down also move one item at a time.

It applies to:
- **Database:** album and track lists. An album's cover comes from its first track.
- **File browser:** any folder with music or playlists in it. A folder that only contains other folders stays a text list.
- **Playlists:** the playlist catalogue.
- **Audiobooks:** every book list. The second line shows the author and progress.

Artists aren't covered yet.

Items without a cover show a tile with their first letter. Covers are decoded the first time they appear on screen, and the last twelve are kept in a cache.

## Playlist covers

A saved playlist (`.m3u` / `.m3u8`) uses an image with the same name in the same folder:

```
Playlists/Road Trip.m3u8
Playlists/Road Trip.jpg      (or .jpeg, .png)
```

If there is no such image, the playlist uses the cover of its first track. Covers are always drawn square. Dynamic playlists (database queries, the current queue) have no cover.

`.gif` and `.webp` covers are drawn by the video decoder (see [Video](video.md)): in a list they show their first frame, and a WPS `%Cp` animates them. Without the decoder installed they count as no cover. An animated `.png` shows its first frame.

### In a WPS

| Tag | What it does |
|---|---|
| `%Cp(x, y, size)` | Draws the cover of the playlist that is playing, as a square |
| `%?CP<yes\|no>` | True when the playing playlist has a cover (its own image or a first track) |

```
%V(30,100,-30,220,-)
%?CP<%Cp(0,0,200)|>
```

## For developers

`gui_synclist` now has a `callback_draw_list` hook, which draws the whole list area in place of the rows. `apps/gui/coverview.c` uses it for the layouts above.

A list opts in with `coverview_attach(list, &source, style)`. The `coverview_source` provides two callbacks: one draws an item's cover, the other returns its second line. The layouts don't know what the items are, so artists, or anything else with a picture, can be added with a new source.

Covers are loaded from a path by `apps/gui/covers.c`. The path can be an image file, a playlist, or an audio file (embedded art first, then folder art).
