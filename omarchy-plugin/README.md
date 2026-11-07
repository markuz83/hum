# Hum visualizer

An [Omarchy](https://omarchy.org) bar widget that draws Hum's live
spectrum while it plays, with a thin progress line and, if you want it,
the track's name scrolling beside the bars. It disappears when Hum isn't running.

| Action | Result |
|---|---|
| Click | Open or focus Hum |
| Click on the progress line (the bottom edge of the bar) | Seek to that point |
| Middle-click | Play / pause |
| Right-click | Next track |
| Scroll | Volume ±5% |
| Hover | Title and artist |

## Install

```sh
hum plugin install --enable
```

That copies the plugin to `~/.config/omarchy/plugins/markm.hum-visualizer`, enables it, and
places it on the right of the bar. Move it with `omarchy bar move markm.hum-visualizer --section left`.
Set the number of bars (4 to 24) with `omarchy bar set markm.hum-visualizer bars 16`.

| Setting | Default | |
|---|---|---|
| `bars` | 12 | Number of bars, 4 to 24 |
| `progress` | true | The progress line |
| `showTitle` | false | The track name, scrolling when it is long |
| `titleWidth` | 160 | Most room the track name takes, in pixels |

Booleans need `--json`: `omarchy bar set markm.hum-visualizer showTitle true --json`.

## How it works

Hum listens on `$XDG_RUNTIME_DIR/hum-bar.sock` (owner-only) and, only while a client is connected,
streams one line per message: `S Playing|Paused|Stopped`, `N <title>\t<artist>`, `L <n> <n> …`
(24 spectrum values, 0 to 99, about 30 a second) and `P <position> <duration>` (milliseconds, twice
a second). The widget sends back `playpause`, `next`, `previous`, `volume +5` / `volume -5` or `seek <ms>`. The protocol is in `src/barlink.h`; `Protocol.js` parses it,
bounds every value, and is covered by `node --test omarchy-plugin/tests`.

Titles come from file tags, so they are untrusted: they are stored as text and shown only in the
shell's tooltip, which draws plain text.
