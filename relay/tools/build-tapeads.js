// Builds an ad pool from archive.org tapes of whole commercial breaks (off-air recordings,
// toy-commercial reels): ffmpeg's blackdetect finds the black frames stations put between
// spots, and each stretch between two of them that is commercial-length becomes one ad
// (a slice of the tape: file + start + length).
//   node tools/build-tapeads.js tapeads-80s-kids.json <item> <item> ...
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const [outName, ...items] = process.argv.slice(2);
if (!outName || !items.length) { console.error('usage: build-tapeads.js out.json item...'); process.exit(1); }
const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/', outName);
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const MIN_S = 9, MAX_S = 65, PARALLEL = 8;

async function getJson(url, tries = 4) {
  for (let i = 1; ; i++) {
    try {
      const r = await fetch(url, { headers: UA, signal: AbortSignal.timeout(60000) });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return await r.json();
    } catch (e) { if (i >= tries) throw e; await new Promise(res => setTimeout(res, 2000 * i)); }
  }
}

function blacks(url) {
  return new Promise(resolve => {
    const ff = spawn('ffmpeg', ['-hide_banner', '-nostats', '-user_agent', UA['user-agent'], '-i', url, '-an',
      '-vf', 'fps=10,scale=160:-2,blackdetect=d=0.08:pix_th=0.12', '-f', 'null', '-'], { stdio: ['ignore', 'ignore', 'pipe'] });
    let err = '';
    ff.stderr.on('data', d => { err += d; });
    ff.on('close', () => {
      const out = [];
      for (const m of err.matchAll(/black_start:([\d.]+) black_end:([\d.]+)/g)) out.push([+m[1], +m[2]]);
      resolve(out);
    });
  });
}

async function tape(item) {
  const d = await getJson(`https://archive.org/metadata/${item}`);
  const vids = (d.files || []).filter(f => Number(f.length) > 60 && /\.(mp4|m4v)$/i.test(f.name) && !/\.ia\.mp4$/i.test(f.name));
  const ads = [];
  const title = String(d.metadata?.title || item).replace(/\s+/g, ' ').slice(0, 80);
  for (const f of vids) {
    const url = `https://archive.org/download/${item}/${f.name.split('/').map(encodeURIComponent).join('/')}`;
    const b = await blacks(url);
    const len = Number(f.length);
    // Spots run between the end of one black stretch and the start of the next.
    const cuts = [[0, 0], ...b, [len, len]];
    for (let i = 0; i + 1 < cuts.length; i++) {
      const start = cuts[i][1], end = cuts[i + 1][0], dur = end - start;
      if (dur >= MIN_S && dur <= MAX_S) ads.push({ id: item, file: f.name, inS: Math.round(start * 100) / 100, durationS: Math.round(dur * 100) / 100, title });
    }
  }
  console.log(`${item}: ${ads.length} spots`);
  return ads;
}

// Results are saved after every tape, in item order, so stopping early keeps what's done.
const done = new Map();
const save = () => fs.writeFileSync(OUT, JSON.stringify(items.flatMap(it => done.get(it) || []), null, 1) + '\n');
let next = 0;
await Promise.all(Array.from({ length: PARALLEL }, async () => {
  while (next < items.length) {
    const it = items[next++];
    done.set(it, await tape(it).catch(e => (console.warn(it, e.message), [])));
    save();
  }
}));
const out = items.flatMap(it => done.get(it) || []);
console.log(`${out.length} spots -> ${path.relative(process.cwd(), OUT)}`);
