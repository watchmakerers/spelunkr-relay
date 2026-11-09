# SPELUNKR relay and clients

A cable-TV style way to watch [spelunkr.tv](https://spelunkr.tv) on a PlayStation 3, a PS Vita,
or a Windows / macOS / Linux computer: a channel guide you drive with a controller (or
keyboard), channel surfing, and the same synchronized schedule the website plays, plus a few
channels of our own.

Consoles can't run the site, so the work is split in two:

```
 spelunkr.tv  ──lineup + manifests──▶  relay (your PC)  ──guide + MPEG-1 over LAN──▶  PS3 / Vita / desktop app
 archive.org  ──────videos──────────▶   Node + ffmpeg
```

- **`relay/`** runs Spelunkr's *own* scheduling engine, extracted from the site's bundle at
  startup (with a vendored fallback), so what's on matches the website. It serves a compact
  guide and streams each schedule entry (shows, commercials, bumpers, up-next cards) as
  MPEG-1 video + MP2 audio, which every client decodes in software. Audio is loudness-
  normalised (EBU R128), so all channels play at the same level.
- **`core/`** is the client itself, shared by every platform: guide grid with picture-in-
  picture, channel banner, pixel channel logos, controls, streaming and decoding (pl_mpeg).
- **`ps3/`** (PSL1GHT: RSX shaders, PS3 audio and pads), **`sdl/`** (SDL2: used by both
  **`desktop/`** and **`vita/`**) are the thin platform layers (`core/platform.h`).

## Run the relay (on a PC on the same network)

Needs Node 20+ and ffmpeg.

```
cd relay
npm start            # http://<pc>:8642/ shows what's on; /watch/<channel> plays in mpv/VLC
```

Clients find it automatically: a UDP broadcast on port 47555 first, then (desktop and Vita)
a quick scan of the local network for port 8642, which works even where a firewall drops
broadcast replies. The address found is remembered (`relay-found.txt` in the data folder).
The relay's PC must accept 8642/tcp (and 47555/udp for the broadcast; with ufw:
`sudo ufw allow 8642/tcp && sudo ufw allow 47555/udp`). You can always give clients the
address directly, as described below.

## The apps

Every push builds all of them on GitHub Actions (see the *Actions* tab for downloads);
pushing a `v*` tag publishes them as a release.

| | Get it | Relay address, if it isn't found |
|---|---|---|
| **PS3** (HEN/CFW) | `ps3/spelunkr.pkg`: install with Package Manager | `/dev_hdd0/game/SPLK00001/USRDIR/relay.txt` |
| **PS Vita** (HENkaku/Ensō) | `spelunkr.vpk`: install with VitaShell | `ux0:data/spelunkr/relay.txt` |
| **Windows / macOS / Linux** | `spelunkr-*.zip` / `.tar.gz` | `spelunkr --relay 192.168.x.y:8642`, or `relay.txt` in the data folder (`spelunkr --help` shows where) |

`relay.txt` holds one line like `192.168.1.20:8642`.

Desktop extras: `--fullscreen` (F11 / Alt+Enter toggles), `--4x3` (the SD-TV layout the PS3
uses on a CRT). The macOS build isn't signed: right-click → Open the first time. The Linux
build needs SDL2 installed (it is on most desktops).

### Build them yourself

```
ps3/build.sh                                           # Docker (PS3 toolchain image) -> ps3/spelunkr.pkg
cmake -S desktop -B build/desktop && cmake --build build/desktop    # needs SDL2 dev files
cmake -S vita -B build/vita && cmake --build build/vita             # needs VitaSDK -> spelunkr.vpk
```

(`-DSPELUNKR_VENDOR_SDL=ON` builds SDL2 from source and links it statically, as the Windows
and macOS releases do. For the Vita without installing VitaSDK:
`docker run --rm -v "$PWD":/src -w /src vitasdk/vitasdk sh -c "cmake -S vita -B build/vita && cmake --build build/vita"`.)

## Relay-only channels

Besides Spelunkr's lineup, the relay can run channels of its own (`relay/src/custom.js`).
Each loops a list of archive.org videos on a clock-anchored, seeded-shuffle schedule, so
every viewer sees the same thing at the same time, just like the real channels. Between
episodes they run Spelunkr-style breaks: a station break with the channel's own slides,
2-3 commercials, a station break with Spelunkr's slides (`channels/spelunkr-platform.json`,
a snapshot of the site's), and an "up next" card.

- **Channel 4 - X-Play**: G4's X-Play, 161 full episodes from 2008-2012 (~59 hours of
  episodes). The episode list lives in `relay/src/channels/xplay.json`; rebuild it with
  `node tools/build-xplay.js` (in `relay/`) to pick up newly uploaded episodes. Its breaks
  play video game commercials from archive.org (`channels/gameads.json`, built by
  `node tools/build-gameads.js`) and its own slides (`channels/xplay-bumps.json`). The 4:3
  episodes (2008-09) air four times per cycle, evenly spaced; the widescreen ones once.
- **Channel 5 - Classic MTV**: Jackass, Beavis and Butt-Head, Celebrity Deathmatch, Viva La
  Bam and Headbangers Ball (~135 hours; `channels/mtv.json`, `node tools/build-mtv.js`), with
  real 1993-2005 US TV commercials in the breaks (`channels/tvads-9300s.json`,
  `node tools/build-tvads.js`). Multi-part Headbangers Ball broadcasts stay in order.

## Controls

| | PS3 / Vita | Keyboard | Game controller |
|---|---|---|---|
| Channel up / down (watching) | ↑ / ↓ | ↑ / ↓ | D-pad / stick |
| Show the channel banner | ✕ or □ | Enter, I or Space | A or X |
| Open the guide | △, ○ or START | G, Tab or Esc | Y, B or Start |
| Guide: move | ↑ ↓ ← → | arrow keys | D-pad / stick |
| Guide: page | L1 / R1 (Vita: L / R) | PgUp / PgDn | LB / RB |
| Guide: watch selected | ✕ | Enter | A |
| Guide: close | △ or ○ | Esc or G | B or Y |
| Quit (desktop) | | Ctrl+Q, or close the window | |

## Notes

- The relay reads Spelunkr's public broadcast files from `spelunkr.tv/broadcast/` (the site's
  `data.spelunkr.tv` host is behind a bot check) and video from archive.org, the same sources
  your browser uses. Shows are transcoded on the fly; one channel uses one ffmpeg.
- Tuning takes a few seconds while the relay seeks into the archive.org tape.
- `core/tools/decode_test.c` tests pl_mpeg against relay output on the host. The desktop
  app's `--shot FILE SECONDS [--shot-guide]` saves a screenshot and quits (run it with
  `SDL_VIDEODRIVER=offscreen SDL_RENDER_DRIVER=software` for headless checks).
- Font: Space Mono Bold (SIL OFL, `core/font/OFL-SpaceMono.txt`).
