### Keymap: the five physical keys

The R1's five buttons mean the same thing on every screen. They are resolved
globally in `apps/rpkeys.c`, before any keymap context, so none of them
navigates a list or a menu - the stick and the touchscreen do that.

| Key | Press | Action |
| :--- | :--- | :--- |
| **Power** | tap | Play / pause on the WPS; from any other screen, go to the WPS |
| | hold 0.5 s | Lock / unlock the touchscreen, on release |
| | hold 2 s | Screen off, on release; any press wakes it |
| | hold 4 s | Shut down |
| **Vol Up / Vol Down** | tap | Four volume steps (2 dB) |
| | hold | Continuous volume change |
| **Next** | hold | Previous track |
| **Play** | hold | Next track |
| **Power** + **Vol Up** | | Screenshot, on any screen, locked or not |

The R1 has no previous key, so the two transport keys are previous and next
the way they fall under the thumb, whatever the silkscreen says. Play / pause
is the stick's centre tap. `BUTTON_PREV` and `BUTTON_NEXT` exist on this
target for Bluetooth remotes only.

In the Bluetooth receiver view (a PC or phone playing to the R1) the Power
tap, the volume keys and the track keys control the sender instead.

#### Notes

- The screenshot is saved as a JPEG in `/screenshots` on the card, named for
  the time in UTC plus a random suffix -
  `2026-09-22-04-20-20-utc0-odml2.jpg` - with a `.txt` of the display
  present stats beside it. The screen flashes white, then shows a thumbnail
  of the shot bottom left for about 2 s.
- With the screen off, a Power press only wakes it, however long it is held.
