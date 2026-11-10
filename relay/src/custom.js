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

// A chunky 3-D "M" with a little gold "tv" tucked under its right leg.
const M_LOGO = (() => {
  const g = Array.from({ length: 32 }, () => Array(32).fill('.'));
  const put = (x, y, ch) => { if (x >= 0 && x < 32 && y >= 0 && y < 32) g[y][x] = ch; };
  const M = (x, y) => {
    if (y < 5 || y > 22 || x < 4 || x > 25) return false;
    if (x <= 8 || x >= 21) return true;                     // the two legs
    const d = y - 5;                                        // the V, 3px thick
    return (x - 9 >= d - 2 && x - 9 <= d + 1 && x <= 15) || (20 - x >= d - 2 && 20 - x <= d + 1 && x >= 14);
  };
  for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) if (M(x - 2, y - 2) && !M(x, y)) put(x, y, 'd'); // extrusion
  for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) if (M(x, y)) put(x, y, 'c');
  for (let y = 0; y < 32; y++) for (let x = 0; x < 32; x++) if (M(x, y) && !M(x - 1, y)) put(x, y, 'l'); // highlight
  const tv = ['ggg.g.g', '.g..g.g', '.g..g.g', '.g...g.'];
  tv.forEach((r, j) => [...r].forEach((ch, i) => ch === 'g' && put(20 + i, 25 + j, 'g')));
  return { type: 'pixel', rows: g.map(r => r.join('')) };
})();

// A striped 80s sunset over a neon grid.
const SUNSET_LOGO = (() => {
  const g = Array.from({ length: 32 }, () => Array(32).fill('.'));
  for (let y = 2; y < 30; y++) for (let x = 2; x < 30; x++) g[y][x] = (x <= 3 || x >= 28 || y <= 3 || y >= 28) ? 'k' : 'n';
  const cx = 15.5, cy = 18, r = 10;
  for (let y = 5; y < 19; y++) for (let x = 4; x < 28; x++) {
    if ((x - cx) ** 2 + (y - cy) ** 2 > r * r) continue;
    if (y >= 13 && (y - 13) % 3 === 2) continue; // the stripes cut through the lower half
    g[y][x] = y < 10 ? 'g' : y < 14 ? 'o' : 'c';
  }
  for (let y = 19; y < 28; y++) for (let x = 4; x < 28; x++) {
    const horizon = y === 19 || y === 21 || y === 24;
    const ray = [-12, -6, 0, 6, 12].some(k => Math.round(cx + k * (y - 18) / 9) === x); // grid lines to the horizon
    g[y][x] = horizon || ray ? 'l' : 'k';
  }
  return { type: 'pixel', rows: g.map(r => r.join('')) };
})();

export const DEFINITIONS = [
  {
    id: 'relay-xplay', dial: 4, name: 'X-Play', color: '#d2232a',
    description: "G4's X-Play, around the clock: reviews, previews and sketches from the 4:3 years, 2008-2009. Gaming ads in the breaks.",
    logomark: X_LOGO, seed: 44,
    shows: ['channels/xplay.json'], bumps: 'channels/xplay-bumps.json', ads: 'channels/gameads.json', adsPerBreak: [2, 3],
  },
  {
    id: 'relay-mtv', dial: 5, name: 'Classic MTV', color: '#f2c200',
    description: 'Jackass, Beavis and Butt-Head, Celebrity Deathmatch, Viva La Bam and Headbangers Ball, with real 90s and 2000s commercials.',
    logomark: M_LOGO, seed: 1997,
    shows: ['channels/mtv.json'], bumps: 'channels/mtv-bumps.json', ads: 'channels/tvads-9300s.json', adsPerBreak: [2, 4],
  },
  {
    id: 'relay-toons', dial: 6, name: 'Toon Rewind', color: '#e0218a',
    description: "80s and early-90s cartoons: He-Man, ThunderCats, Turtles, G.I. Joe, Transformers, DuckTales, Gargoyles and more, with Saturday-morning commercials.",
    logomark: SUNSET_LOGO, seed: 1985,
    shows: ['channels/toons.json'], bumps: 'channels/toons-bumps.json',
    ads: ['channels/tapeads-80s-kids.json', 'channels/tvads-80s-kids.json'], adsPerBreak: [3, 4],
    spread: true, // no show twice in a row, except a multi-part story
  },
];

/**
 * Episode order for one cycle. Multi-part broadcasts (same `group`) stay together and in
 * part order. Units with repeat k > 1 air k times: the rest are split into k stretches
 * and each stretch gets one copy of every repeated unit, so repeats stay far apart.
 */
function order(episodes, rand, repeat, spread) {
  const units = [], byGroup = new Map();
  for (const e of episodes) {
    if (!e.group) { units.push([e]); continue; }
    if (!byGroup.has(e.group)) { const u = []; byGroup.set(e.group, u); units.push(u); }
    byGroup.get(e.group).push(e);
  }
  for (const u of byGroup.values()) u.sort((a, b) => (a.part ?? 0) - (b.part ?? 0));
  const times = u => Math.max(1, Math.floor(repeat ? repeat(u[0]) : 1));
  const k = Math.max(1, ...units.map(times));
  const mix = list => (spread ? spreadShows(shuffle(list, rand)) : shuffle(list, rand));
  const once = mix(units.filter(u => times(u) === 1));
  const again = shuffle(units.filter(u => times(u) > 1), rand); // same order in every stretch
  const out = [];
  for (let r = 0; r < k; r++) {
    const stretch = once.slice(Math.floor((r * once.length) / k), Math.floor(((r + 1) * once.length) / k));
    const extra = again.filter(u => times(u) > r);
    // drop the repeated units in at even spacing, at the same spots each stretch, so each
    // one comes back about a stretch later instead of possibly right after itself
    const merged = [];
    let j = 0;
    for (let i = 0; i < stretch.length + extra.length; i++) {
      const due = j < extra.length && i >= Math.round(((j + 0.5) * (stretch.length + extra.length)) / extra.length - 0.5);
      merged.push(due ? extra[j++] : stretch[i - j]);
    }
    out.push(...merged);
  }
  return out.flat();
}

/**
 * Reorders shuffled units so no show follows itself: each time the next unit is the same
 * show as the last one placed, the nearest later unit of another show moves up instead.
 */
function spreadShows(units) {
  const out = [], rest = units.slice();
  while (rest.length) {
    const last = out.length ? out[out.length - 1][0].show : null;
    let i = rest.findIndex(u => u[0].show !== last);
    if (i < 0) i = 0; // only that show is left
    out.push(rest.splice(i, 1)[0]);
  }
  return out;
}

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
    const { shows, bumps, ads, adsPerBreak = [2, 3], seed, repeat, spread, ...meta } = def;
    this.meta = meta;
    const rand = rng(seed);
    const platform = read('channels/spelunkr-platform.json');
    const episodes = order(shows.flatMap(read), rand, repeat, spread);
    const channelBump = deck(bumps ? read(bumps) : [], rand);
    const platformBump = deck(platform.bumps, rand);
    const song = deck(platform.audio, rand);
    const ad = deck([].concat(ads || []).flatMap(f => (fs.existsSync(path.join(HERE, f)) ? read(f) : [])), rand);
    const tune = () => { const a = song(); return a && { filename: a.filename, startOffset: Math.floor(rand() * 20) }; };

    this.items = [];
    const push = (it, durMs) => this.items.push({ ...it, durMs: Math.round(durMs) });
    for (const e of episodes) {
      push({ type: 'video', id: e.id, file: e.file, title: e.title, year: e.year, description: e.description, ...(e.crop ? { crop: e.crop } : {}) }, e.durationS * 1000);
      const open = channelBump();
      if (open) push({ type: 'bump-break', half: 'open', channelBump: open, channelAudio: tune() }, breakHalfMs('open', open.slides));
      const n = adsPerBreak[0] + Math.floor(rand() * (adsPerBreak[1] - adsPerBreak[0] + 1));
      for (let k = 0; k < n; k++) {
        const a = ad();
        if (!a) break;
        push({ type: 'ad', title: a.title, slice: { tape: a.id, file: a.file, inMs: Math.round((a.inS || 0) * 1000), lengthMs: Math.round(a.durationS * 1000) } }, a.durationS * 1000);
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
