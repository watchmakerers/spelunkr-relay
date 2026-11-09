// Relay-only channels: schedules the relay makes itself, alongside Spelunkr's lineup.
//
// A custom channel loops a fixed list of archive.org videos on a clock-anchored cycle, so
// like the real channels every viewer (and every PS3) sees the same thing at the same time.
// The order is shuffled once with a fixed seed, so reruns come round in a TV-like order
// rather than strictly by date. Entries have the same shape as the Spelunkr engine's.

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const ANCHOR_MS = Date.UTC(2026, 0, 1); // the cycle starts here; any fixed date works

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

const DEFINITIONS = [
  {
    id: 'relay-xplay', dial: 4, name: 'X-Play', color: '#d2232a',
    description: "G4's X-Play, around the clock: reviews, previews and sketches, 2008-2012.",
    logomark: X_LOGO, episodes: 'channels/xplay.json', seed: 4,
  },
];

function shuffle(list, seed) {
  const a = list.slice();
  let s = seed >>> 0 || 1;
  const rand = () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32);
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

export class CustomChannel {
  constructor(def) {
    const { episodes, seed, ...meta } = def;
    this.meta = meta;
    const list = JSON.parse(fs.readFileSync(path.join(HERE, episodes), 'utf8'));
    this.items = shuffle(list, seed).map(e => ({
      type: 'video', id: e.id, file: e.file, title: e.title, year: e.year,
      description: e.description, durMs: Math.round(e.durationS * 1000),
    }));
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
      const e = this.entry(cycleStart, index, fromMs);
      yield e;
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
