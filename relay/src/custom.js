// Relay-only channels: schedules the relay makes itself, alongside Spelunkr's lineup.
//
// A custom channel loops a fixed list of archive.org videos on a clock-anchored cycle, so
// like the real channels every viewer (and every PS3) sees the same thing at the same time.
// Between episodes it runs a Spelunkr-style break:
//
//   episode -> station break (channel ident + channel slides, over music)
//           -> 2-3 commercials -> station break (Spelunkr slides) -> "up next" card -> episode
//
// The whole cycle (episode order, which slides, songs and ads) is laid out once from a
// fixed seed, so it is identical on every relay and every run. Entries have the same shape
// as the Spelunkr engine's, so render.js draws the breaks exactly like the real channels.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { breakHalfMs } from './render.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ANCHOR_MS = Date.UTC(2026, 0, 1); // the cycle starts here; any fixed date works
const UP_NEXT_MS = 8000;
const read = f => JSON.parse(fs.readFileSync(path.join(HERE, f), 'utf8'));

// 32x32 logomark in the site's pixel-logo letters: c = channel colour, d = its shade,
// p = white, k = near-black, g = gold. A chunky "X" with a gold underline.
const X_LOGO = (() => {
  const rows = [];
  for (let y = 0; y < 32; y++) {
    let r = '';
    for (let x = 0; x < 32; x++) {
      const inBox = x >= 2 && x <= 29 && y >= 2 && y <= 29;
      const border = inBox && (x <= 3 || x >= 28 || y <= 3 || y >= 28);
      const u = x - 6, v = y - 5; // X spans 6..25 x 5..24
      const onX = u >= 0 && u < 20 && v >= 0 && v < 20 && (Math.abs(u - v) <= 2 || Math.abs(u + v - 19) <= 2);
      const shadow = !onX && u - 1 >= 0 && u - 1 < 20 && v - 1 >= 0 && v - 1 < 20 &&
        (Math.abs(u - 1 - (v - 1)) <= 2 || Math.abs(u - 1 + v - 1 - 19) <= 2);
      const bar = y >= 25 && y <= 26 && x >= 8 && x <= 23;
      r += !inBox ? '.' : border ? 'k' : onX ? 'p' : shadow ? 'd' : bar ? 'g' : 'c';
    }
    rows.push(r);
  }
  return { type: 'pixel', rows };
})();

export const DEFINITIONS = [
  {
    id: 'relay-xplay', dial: 4, name: 'X-Play', color: '#d2232a',
    description: "G4's X-Play, around the clock: reviews, previews and sketches, 2008-2012. Gaming ads in the breaks.",
    logomark: X_LOGO, seed: 4,
    shows: ['channels/xplay.json'], bumps: 'channels/xplay-bumps.json', ads: 'channels/gameads.json', adsPerBreak: [2, 3],
  },
];

function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32);
}
function shuffle(list, rand) {
  const a = list.slice();
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}
/** Draws from a shuffled list without repeats until it runs out, then reshuffles. */
function deck(list, rand) {
  let order = [], i = 0;
  return () => {
    if (!list.length) return null;
    if (i >= order.length) { order = shuffle(list, rand); i = 0; }
    return order[i++];
  };
}

export class CustomChannel {
  constructor(def) {
    const { shows, bumps, ads, adsPerBreak = [2, 3], seed, ...meta } = def;
    this.meta = meta;
    const rand = rng(seed);
    const platform = read('channels/spelunkr-platform.json');
    const episodes = shuffle(shows.flatMap(read), rand);
    const channelBump = deck(bumps ? read(bumps) : [], rand);
    const platformBump = deck(platform.bumps, rand);
    const song = deck(platform.audio, rand);
    const ad = deck(ads ? read(ads) : [], rand);
    const tune = () => { const a = song(); return a && { filename: a.filename, startOffset: Math.floor(rand() * 20) }; };

    this.items = [];
    const push = (it, durMs) => this.items.push({ ...it, durMs: Math.round(durMs) });
    for (const e of episodes) {
      push({ type: 'video', id: e.id, file: e.file, title: e.title, year: e.year, description: e.description }, e.durationS * 1000);
      const open = channelBump();
      if (open) push({ type: 'bump-break', half: 'open', channelBump: open, channelAudio: tune() }, breakHalfMs('open', open.slides));
      const n = adsPerBreak[0] + Math.floor(rand() * (adsPerBreak[1] - adsPerBreak[0] + 1));
      for (let k = 0; k < n; k++) {
        const a = ad();
        if (!a) break;
        push({ type: 'ad', title: a.title, slice: { tape: a.id, file: a.file, inMs: 0, lengthMs: Math.round(a.durationS * 1000) } }, a.durationS * 1000);
      }
      const close = platformBump();
      if (close) push({ type: 'bump-break', half: 'close', platformBump: close, platformAudio: tune() }, breakHalfMs('close', close.slides));
      const sting = song();
      push({ type: 'up-next', sting: sting && { track: sting.filename, startMs: 0 } }, UP_NEXT_MS);
    }
    this.starts = [];
    let t = 0;
    for (const it of this.items) { this.starts.push(t); t += it.durMs; }
    this.cycleMs = t;
  }

  /** Entry `index` of the cycle that begins at `cycleStart`. */
  entry(cycleStart, index, ms) {
    const item = this.items[index];
    const startMs = cycleStart + this.starts[index];
    const endMs = startMs + item.durMs;
    const offsetExact = Math.max(0, (ms - startMs) / 1000);
    return {
      item, startMs, endMs, durMs: item.durMs,
      offsetExact, offset: Math.floor(offsetExact), remaining: (endMs - Math.max(ms, startMs)) / 1000,
    };
  }

  locate(ms) {
    const since = ms - ANCHOR_MS;
    const cycle = Math.floor(since / this.cycleMs);
    const pos = since - cycle * this.cycleMs;
    let lo = 0, hi = this.starts.length - 1;
    while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (this.starts[mid] <= pos) lo = mid; else hi = mid - 1; }
    return { cycleStart: ANCHOR_MS + cycle * this.cycleMs, index: lo };
  }

  at(ms) {
    const { cycleStart, index } = this.locate(ms);
    return this.entry(cycleStart, index, ms);
  }

  /** Entries from the one on air at `fromMs` onwards, forever. */
  *walk(fromMs) {
    let { cycleStart, index } = this.locate(fromMs);
    for (;;) {
      yield this.entry(cycleStart, index, fromMs);
      if (++index === this.items.length) { index = 0; cycleStart += this.cycleMs; }
    }
  }
}

export function loadCustomChannels(log = console) {
  const out = [];
  for (const def of DEFINITIONS) {
    try { out.push(new CustomChannel(def)); } catch (e) { log.warn(`[custom] ${def.id}: ${e.message}`); }
  }
  return out;
}
