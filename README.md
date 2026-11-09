# SPELUNKR for PS3

A cable-TV style client for [spelunkr.tv](https://spelunkr.tv) on a PlayStation 3 (HEN or CFW):
a channel guide you drive with the controller, channel surfing, and the same synchronized
schedule the website plays.

The PS3's browser can't run the site, so the work is split in two:

```
 spelunkr.tv  ──lineup + manifests──▶  relay (your PC)  ──guide + MPEG-1 over LAN──▶  PS3 app
 archive.org  ──────videos──────────▶   Node + ffmpeg
```

- **`relay/`** runs Spelunkr's *own* scheduling engine, extracted from the site's bundle at
  startup (with a vendored fallback), so what's on matches the website. It serves a compact
  guide and streams each schedule entry (shows, commercials, bumpers, up-next cards) as
  MPEG-1 video + MP2 audio, a format the PS3 can decode in software.
- **`ps3/`** is a PSL1GHT homebrew app: guide grid with picture-in-picture, channel banner,
  pixel channel logos, and a player (pl_mpeg decode, YUV→RGB on the RSX, audio on its own thread).

## Run the relay (on a PC on the same network)

Needs Node 20+ and ffmpeg.

```
cd relay
npm start            # http://<pc>:8642/ shows what's on; /watch/<channel> plays in mpv/VLC
```

The PS3 finds it automatically (UDP broadcast on port 47555). If your network blocks that, put
`192.168.x.y:8642` in `/dev_hdd0/game/SPLK00001/USRDIR/relay.txt` on the PS3.

## Build and install the PS3 app

```
cd ps3
./build.sh           # Docker; or `make pkg` with PSL1GHT installed
```

Install `ps3/spelunkr.pkg` with Package Manager (HEN/CFW). It appears in the XMB as SPELUNKR.

## Relay-only channels

Besides Spelunkr's lineup, the relay can run channels of its own (`relay/src/custom.js`).
Each loops a list of archive.org videos on a clock-anchored, seeded-shuffle schedule, so
every viewer sees the same thing at the same time, just like the real channels.

- **Channel 4 - X-Play**: G4's X-Play, 161 full episodes from 2008-2012 (~59 hours per
  cycle). The episode list lives in `relay/src/channels/xplay.json`; rebuild it with
  `node tools/build-xplay.js` (in `relay/`) to pick up newly uploaded episodes.

## Controls

| Watching | |
|---|---|
| ↑ / ↓ | channel up / down |
| ✕ or □ | show the channel banner |
| △, ○ or START | open the guide |

| Guide | |
|---|---|
| ↑ / ↓ | channel |
| ← / → | earlier / later show |
| L1 / R1 | page up / down |
| ✕ | watch the selected channel |
| △ or ○ | close the guide |

## Notes

- The relay reads Spelunkr's public broadcast files from `spelunkr.tv/broadcast/` (the site's
  `data.spelunkr.tv` host is behind a bot check) and video from archive.org, the same sources
  your browser uses. Shows are transcoded on the fly; one channel uses one ffmpeg.
- Tuning takes a few seconds while the relay seeks into the archive.org tape.
- `ps3/tools/` has host-side tests: `decode_test.c` (pl_mpeg against relay output) and
  `preview.c` (renders the PS3 screens to images using the app's real UI code).
- Font: Space Mono Bold (SIL OFL, `ps3/data/OFL-SpaceMono.txt`).
