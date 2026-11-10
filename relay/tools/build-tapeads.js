// Builds an ad pool from archive.org tapes of whole commercial breaks (off-air recordings,
// toy-commercial reels): ffmpeg's blackdetect finds the black frames stations put between
// spots, and each stretch between two of them that is commercial-length becomes one ad
// (a slice of the tape: file + start + length). Scans use archive.org's small copy of
// each video; spots play from the original upload.
//   node tools/build-tapeads.js tapeads-80s-kids.json <item> <item> ...
import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const [outName, ...items] = process.argv.slice(2);
if (!outName || !items.length) { console.error('usage: build-tapeads.js out.json item...'); process.exit(1); }
const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/', outName);
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const MIN_S = 9, MAX_S = 65, PARALLEL = Number(process.env.PARALLEL) || 4;
// Spots play from the uploader's original file, which is usually far better than
// archive.org's 640x480 ~800 kbps re-encode, unless it's too heavy to stream live.
const MAX_PLAY_KBPS = 12000;
const VIDEO = /\.(mp4|m4v|mkv|webm|mov|avi|mpe?g|ts)$/i;
// Tapes whose picture sits inside a black border (an ffmpeg crop, found with cropdetect).
const CROP = { 'jan-9th-1985-channel-5-and-13-new-york-he-man-raccons-on-ice-she-ra-commercials': 'iw*0.82:ih*0.98' };
const kbps = f => (f.size && f.length ? f.size * 8 / f.length / 1000 : 0);

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

// The first timestamp of an MPEG program stream (read from its start, no seeking): the
// relay reads .mpg spots by byte range and trims on the file's own timestamps.
function startTime(url) {
  return new Promise(resolve => {
    const fp = spawn('ffprobe', ['-v', 'error', '-user_agent', UA['user-agent'], '-seekable', '0',
      '-show_entries', 'format=start_time', '-of', 'csv=p=0', url], { stdio: ['ignore', 'pipe', 'ignore'] });
    let out = '';
    fp.stdout.on('data', d => { out += d; });
    fp.on('close', () => { const t = parseFloat(out); resolve(Number.isFinite(t) ? t : null); });
  });
}

async function tape(item) {
  const d = await getJson(`https://archive.org/metadata/${item}`);
  const files = (d.files || []).filter(f => Number(f.length) >= MIN_S && VIDEO.test(f.name));
  const originals = files.filter(f => f.source === 'original');
  const ads = [];
  const title = String(d.metadata?.title || item).replace(/\s+/g, ' ').slice(0, 80);
  const url = f => `https://archive.org/download/${item}/${f.name.split('/').map(encodeURIComponent).join('/')}`;
  const one = async o => {
    // archive.org's own copy of the same video (same timing), smallest first: cheap to scan.
    const copy = files.filter(f => f.source === 'derivative' && f.original === o.name && /\.mp4$/i.test(f.name) &&
      Math.abs(Number(f.length) - Number(o.length)) < 1).sort((a, b) => a.size - b.size)[0];
    const f = kbps(o) <= MAX_PLAY_KBPS || !copy ? o : copy;
    const b = await blacks(url(copy || o));
    const len = Number(o.length), out = [];
    const t0 = /\.(mpe?g|vob|m2p)$/i.test(f.name) ? await startTime(url(f)) : null;
    const window = t0 != null ? { t0, size: Number(f.size), lenS: Number(f.length) } : {};
    // Spots run between the end of one black stretch and the start of the next.
    const cuts = [[0, 0], ...b, [len, len]];
    for (let i = 0; i + 1 < cuts.length; i++) {
      const start = cuts[i][1], end = cuts[i + 1][0], dur = end - start;
      if (dur >= MIN_S && dur <= MAX_S) out.push({ id: item, file: f.name, inS: Math.round(start * 100) / 100, durationS: Math.round(dur * 100) / 100, title, ...window, ...(CROP[item] ? { crop: CROP[item] } : {}) });
    }
    return out;
  };
  // A big collection is many files: scan PARALLEL of them at a time, keep file order.
  const per = new Array(originals.length);
  let k = 0;
  await Promise.all(Array.from({ length: PARALLEL }, async () => {
    while (k < originals.length) { const i = k++; per[i] = await one(originals[i]).catch(() => []); }
  }));
  ads.push(...per.flat());
  console.log(`${item}: ${ads.length} spots`);
  return ads;
}

// Results are saved after every tape, in item order, so stopping early keeps what's done.
const done = new Map();
const save = () => fs.writeFileSync(OUT, JSON.stringify(items.flatMap(it => done.get(it) || []), null, 1) + '\n');
let next = 0;
await Promise.all(Array.from({ length: 1 }, async () => {
  while (next < items.length) {
    const it = items[next++];
    done.set(it, await tape(it).catch(e => (console.warn(it, e.message), [])));
    save();
  }
}));
const out = items.flatMap(it => done.get(it) || []);
console.log(`${out.length} spots -> ${path.relative(process.cwd(), OUT)}`);
