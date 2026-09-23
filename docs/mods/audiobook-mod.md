**Listening to audiobooks:**

My audiobook settings file is included with the release - [audiobook-mod-config.cfg](./configs/audiobook-mod-config.cfg)

To activate it, from the Main Menu, open the context menu on Settings -> Browse .cfg files -> open audiobook-mod-config.cfg
Alternatively, you can [download](./configs/audiobook-mod-config.cfg) it manually adjust it and copy it to the SD card. Then just open it in Rockbox in File Browser.

**What it includes:**
* When listening to audiobooks, the Next/Play buttons act as 30-second skips (for music - standard track skipping applies).
* Skip length adjustment for audiobooks has been added to the Quickscreen (top/bottom).
* Playback speed automatically resets when switching from audiobooks to music.
* Recent bookmarks and auto-bookmarks for audiobooks upon stopping playback are enabled.
  * I.e. **Recent Bookmarks** in the **Main Menu** only keep the latest bookmark for each folder. To view all bookmarks for a folder, open the latest bookmark of the desired folder, then open **WPS Context Menu -> Bookmarks -> List Bookmarks** - all bookmarks for that folder will be displayed there.
* Lock turns off the screen instantly, media buttons remain active.
* Rewind across tracks is enabled (skipping to the end of the previous track).
* CUE support is enabled.
* No sleep timer at startup. The original set a 45 minute one on every boot, restarted by every button - a timer nobody had asked for, counting down on the clock. Start one from the quickscreen's Playback page or the audiobook player instead.

**To see chapters in M4B:**
Open the context menu for the M4B file -> *Open with...* -> *mp4chapters_to_cue*. A .cue file with the book's chapters will be generated next to the file.

**IMPORTANT**
Copy audiobooks to the **ABooks** or **Audiobooks** folder (case-sensitive!).
Audiobook folders can be changed: *General Settings -> Alt Settings -> Alt Settings -> Yes* (this will open the folder selection).
To avoid problems with long file names, try to keep paths short.
The Alt Settings menu contains settings that apply only to files from the specified folders.
