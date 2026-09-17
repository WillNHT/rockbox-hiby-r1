# Transitions

Screen changes can be animated. Settings are in **Theme Settings > Transitions**:

| Setting | What it does | Default |
|---|---|---|
| Menu Transition | Entering or leaving a menu, a setting, a folder or a main screen | Slide |
| Menu Transition Time | 50-1000 ms | 200 ms |
| Track Change Transition | The While Playing Screen when the track changes | Fade |
| Track Change Transition Time | 50-1000 ms | 250 ms |

Effects:

- **Off**
- **Fade**: the old screen fades into the new one.
- **Slide**: the new screen slides over the old one. It comes in from the right when you go in (or to the next track) and from the left when you go back (or to the previous track).
- **Push**: the new screen pushes the old one off the screen.
- **Cascade**: the screen arrives in horizontal bands, one after another, so the title, the list rows or the parts of a WPS come in one at a time. It runs top to bottom going in and bottom to top going back.

## Themes

A theme `.cfg` can set any of these, and you can change them afterwards like any other theme setting:

```
menu transition: cascade
menu transition time: 300
wps transition: slide
wps transition time: 250
```

Values: `off`, `fade`, `slide`, `push`, `cascade`.

## How it works

`firmware/drivers/lcd-transition.c`. When a screen is about to change, the code saves what is on the screen and draws the next screen as usual, but doesn't show it yet. When the UI next waits for a key (or sleeps), the new screen is complete, and the old and new frames are animated into each other. If the new screen is identical to the old one, nothing is animated. The animation runs on the UI thread, so input waits until it has finished. Plugins are not animated.
