# Caurdible

Audiobook and music player for the M5Stack Cardputer ADV.

## What it does

- Plays MP3, M4B, M4A and WAV off the SD card.
- Saves your spot every 10 seconds, on pause, and before it powers off.
- Speed from 0.5x to 3x with the pitch kept.
- Skips 30 seconds forward or back.
- Slate and Paper themes.
- Key lock.

## Install

Grab the latest from [Releases](../../releases).

On Launcher, drop `caurdible.bin` in `/bins` on your SD card and install it from there.

## Keys

| Key | What it does |
|---|---|
| `Space` | Play or pause |
| `;` `.` | Up and down. Volume on Now Playing |
| `,` `/` | Left and right. Skips 30s on Now Playing |
| `Enter` | Open or play |
| `Del` or `` ` `` | Back |
| `[` `]` | Previous or next track |
| `-` `=` | Volume |
| `9` `0` | Speed |
| `P` | Now Playing |
| `R` | Recent |
| `S` or `Tab` | Settings |
| `I` | Info |
| `Fn` + `L` | Lock the keys |

## Build

Needs PlatformIO and Python 3.10 to 3.13.

```sh
make build     # firmware
make test      # host tests
make release   # dist/caurdible.bin
```

## Credits

- [cardputer_adv_player](https://github.com/bigbag/cardputer_adv_player) by Pavel Liashkov. MIT.
- Helix AAC decoder by RealNetworks. RPSL. In `lib/libhelix-aac`.
- Sonic by Bill Cox. Apache 2.0. In `lib/sonic`.
- minimp3 by lieff. CC0. In `lib/minimp3`.
- Atkinson Hyperlegible Next and Mono by the Braille Institute. SIL OFL 1.1. In `assets/fonts`.

Caurdible is MIT. See `LICENSE`.
