# Hum visualizer

An [Omarchy](https://omarchy.org) bar widget that draws Hum's live
spectrum while it plays. It disappears when Hum isn't playing.

| Action | Result |
|---|---|
| Click | Open or focus Hum |
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

## How it works

Hum listens on `$XDG_RUNTIME_DIR/hum-bar.sock` (owner-only) and, only while a client is connected,
streams one line per message: `S Playing|Paused|Stopped`, `N <title>\t<artist>`, and `L <n> <n> …`
(24 spectrum values, 0 to 99, about 30 a second). The widget sends back `playpause`, `next`,
`previous` or `volume +5` / `volume -5`. The protocol is in `src/barlink.h`; `Protocol.js` parses it,
bounds every value, and is covered by `node --test omarchy-plugin/tests`.

Titles come from file tags, so they are untrusted: they are stored as text and shown only in the
shell's tooltip, which draws plain text.
