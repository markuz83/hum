# Hum

![Hum playing a track, with the library browser and play queue](docs/screenshot.png)

A music player with a live spectrum visualizer, made for [Omarchy](https://omarchy.org). It plays the
files in your Music folder (MP3, FLAC, Ogg, Opus, M4A and anything else ffmpeg reads), and follows
your desktop: theme colors, font, window opacity, corner rounding and language all come from Omarchy.

## Install

```sh
git clone <this repository> hum && cd hum
bin/install        # builds the package and installs it with pacman
```

Hum then appears in the launcher (Super + Space). `qt6-imageformats` is optional and adds WebP cover images.

## Use

`hum` opens your Music folder (`$XDG_MUSIC_DIR`). `hum <folder>` opens another one, and
`hum <file>` opens its folder and starts that file. Press `O` to pick a folder in the window.

The library browser has three columns, Artist, Album and Title, narrowed from left to right.
Tags come from the files, and anything untagged falls back to the `Artist/Album/Title` folder layout.
Cover art is the embedded picture, or the folder's `cover.jpg`, `folder.jpg`, `front.png` and the like
(a folder with a single image uses that one).

The play queue sits to the right of the browser. Anything you add plays before the library carries on.
Drag the divider, or use `[` and `]`, to resize the two panes; double-click it or press `\` to reset.

### Keys

| Key | Action |
|---|---|
| Space | Play / pause |
| N / P | Next track / previous (restarts the track first if it is more than 3 seconds in) |
| ← → | Seek 5 seconds |
| + − | Volume up / down |
| M | Mute |
| S / R | Shuffle / repeat |
| V | Change visualizer (bars, mirrored, line) |
| F | Visualizer refresh rate (display, 30, 60, 120, 144, 240 Hz) |
| J K ↑ ↓ | Move in the list |
| H L Tab | Switch column |
| Enter | Play the selected track |
| / | Filter the library |
| O | Choose music folder |
| A / Shift+A | Add to queue / play next |
| X Del | Remove from queue |
| Shift+X | Clear the queue |
| Shift+J K, Alt+↑↓ | Move in the queue |
| Ctrl+Z, Ctrl+Shift+Z | Undo / redo queue changes |
| [ ] | Resize the queue pane |
| \ | Reset pane sizes |
| ? | Show every shortcut |
| Q | Quit |

Right-click a row for Play, Play next and Add to queue. The queue also takes drag and drop to reorder.

### The visualizer

The spectrum is computed from the audio actually playing (a 2048-point FFT into 48 log-spaced bands)
and smoothed the way [cava](https://github.com/karlstav/cava) does it. By default it updates once per
frame the display draws; press `F` to lock it to a fixed rate instead. Any rate from 10 to 360 Hz can be
set with `"visualizerRate"` in `~/.config/hum/state.json` (0 follows the display).

### Media keys and the bar

Hum speaks MPRIS, so media keys, `playerctl` and desktop widgets control it.

There is also an Omarchy bar widget that draws a live mini spectrum. Install and enable it with:

```sh
hum plugin install --enable
```

See [omarchy-plugin/README.md](omarchy-plugin/README.md) for its controls and settings.

## Files

| Path | What |
|---|---|
| `~/.config/hum/state.json` | Volume, shuffle, repeat, visualizer, last track and position, queue |
| `~/.cache/hum/tags.json` | Tag and length cache, keyed by file size and modified time |

Hum never changes your music files. State is saved atomically.

## Develop

```sh
bin/build          # qmake6 + make into build/hum
bin/test           # Qt Test suite, run offscreen
node --test omarchy-plugin/tests/protocol.test.js
```

Qt 6 (Quick, Material style, Multimedia with the FFmpeg backend), qmake and a C++17 compiler.
`ffprobe` reads tags and lengths. The code is in `src/`: `backend` holds the library, playback, queue and
state; `analyzer` is the spectrum; `theme` follows Omarchy; `mpris` and `barlink` are the integrations.

## License

MIT, see [LICENSE](LICENSE).
