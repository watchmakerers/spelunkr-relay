// Builds src/channels/weather-music.json: the smooth jazz the weather channel plays under its
// forecasts, from archive.org's TV Tunes uploads of the 80s/90s cable weather "Local
// Forecast" music (tvtunes_26625 to tvtunes_26674).
//   node tools/build-weather-music.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/weather-music.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const FIRST = 26625, LAST = 26674;
const SKIP = /outage|error|superstorm/i; // glitch and storm-coverage reels, not forecast music

const tracks = [];
for (let n = FIRST; n <= LAST; n++) {
  const id = `tvtunes_${n}`;
  const r = await fetch(`https://archive.org/metadata/${id}`, { headers: UA });
  if (!r.ok) continue;
  const d = await r.json();
  const title = String(d.metadata?.title || '').replace(/\s*-\s*Theme Song$/i, '');
  if (!/weather/i.test(title) || SKIP.test(title)) continue;
  const mp3 = (d.files || []).find(f => /\.mp3$/i.test(f.name));
  const len = Number(mp3?.length);
  if (!mp3 || !(len >= 60)) continue;
  tracks.push({ id, file: mp3.name, durationS: Math.round(len * 10) / 10, title });
}
fs.writeFileSync(OUT, JSON.stringify(tracks, null, 1) + '\n');
console.log(`${tracks.length} tracks, ${(tracks.reduce((t, x) => t + x.durationS, 0) / 60).toFixed(0)} min -> ${path.relative(process.cwd(), OUT)}`);
