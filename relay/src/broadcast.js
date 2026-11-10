// Keeps Spelunkr's lineup and channel schedules current, using the site's own engine.
//
// Data comes from https://spelunkr.tv/broadcast/ (the site's static snapshot of its broadcast
// files). The site's browsers prefer data.spelunkr.tv, but that host sits behind a Cloudflare
// bot check that turns away non-browser clients; the snapshot is the same JSON.

import { fetchEngineSource, loadEngine } from './extract.js';
import * as vendored from './engine.vendored.js';
import { loadCustomChannels } from './custom.js';
import { WeatherChannel } from './weather.js';

const DATA = 'https://spelunkr.tv/broadcast/';
const LINEUP_REFRESH_MS = 2 * 60 * 1000;

export class Broadcast {
  constructor({ log = console } = {}) {
    this.log = log;
    this.engine = null;
    this.engineInfo = null;
    this.lineup = null;
    this.channels = new Map(); // id -> { meta, versionKey, playlist }
    this.files = new Map(); // path -> parsed JSON (manifests are immutable per version)
    this.custom = new Map(); // relay-only channels (custom.js): id -> { meta, custom }
    for (const c of loadCustomChannels(log)) this.custom.set(c.meta.id, { meta: c.meta, custom: c });
    try { const w = new WeatherChannel(); this.custom.set(w.meta.id, { meta: w.meta, custom: w }); } catch (e) { log.warn(`[weather] ${e.message}`); }
  }

  async start() {
    await this.loadEngine();
    await this.refresh();
    this.timer = setInterval(() => this.refresh().catch(e => this.log.warn(`[lineup] ${e.message}`)), LINEUP_REFRESH_MS);
    this.timer.unref();
  }

  async loadEngine() {
    try {
      const live = await fetchEngineSource();
      this.engine = loadEngine(live.source);
      this.engineInfo = { from: 'live', build: live.build, maxEngine: live.maxEngine };
    } catch (e) {
      this.log.warn(`[engine] live extraction failed (${e.message}); using the vendored copy from ${vendored.BUILD}`);
      this.engine = loadEngine(vendored.SOURCE);
      this.engineInfo = { from: 'vendored', build: vendored.BUILD, maxEngine: vendored.MAX_ENGINE };
    }
    this.log.info(`[engine] ${this.engineInfo.from} ${this.engineInfo.build} (engine <= ${this.engineInfo.maxEngine})`);
  }

  async json(path, { cache = true } = {}) {
    if (cache && this.files.has(path)) return this.files.get(path);
    const res = await fetch(DATA + path, { cache: 'no-store' });
    if (!res.ok) throw new Error(`${path}: HTTP ${res.status}`);
    const body = await res.json();
    if (cache) this.files.set(path, body);
    return body;
  }

  async refresh() {
    const lineup = await this.json('lineup.json', { cache: false });
    if (this.lineup && lineup.build !== this.lineup.build) {
      this.log.info(`[lineup] site build changed ${this.lineup.build} -> ${lineup.build}; reloading engine`);
      await this.loadEngine();
      for (const ch of this.channels.values()) ch.versionKey = null; // force rebuild with new engine
    }
    this.lineup = lineup;
    const seen = new Set();
    await Promise.all(lineup.channels.map(async meta => {
      seen.add(meta.id);
      const key = meta.versions.map(v => `${v.v}@${v.effectiveAtMs}`).join(',');
      const cur = this.channels.get(meta.id);
      if (cur && cur.versionKey === key) { cur.meta = meta; return; }
      try {
        const versions = [];
        for (const ptr of meta.versions) {
          const manifest = await this.json(ptr.manifest);
          if (this.engineInfo.maxEngine && manifest.engine > this.engineInfo.maxEngine) {
            this.log.warn(`[${meta.id}] manifest needs engine ${manifest.engine}; have ${this.engineInfo.maxEngine}`);
          }
          const platform = manifest.platform ? await this.json(manifest.platform) : null;
          versions.push(this.engine.buildVersion(manifest, platform, ptr));
        }
        this.channels.set(meta.id, { meta, versionKey: key, playlist: this.engine.createPlaylist(meta.id, versions) });
      } catch (e) {
        this.log.warn(`[${meta.id}] could not load schedule: ${e.message}`);
        if (cur) cur.meta = meta;
      }
    }));
    for (const id of this.channels.keys()) if (!seen.has(id)) this.channels.delete(id);
  }

  /** Channels in dial order. */
  list() {
    return [...this.channels.values(), ...this.custom.values()].map(c => c.meta)
      .sort((a, b) => (a.dial ?? 999) - (b.dial ?? 999));
  }

  get(id) { return this.channels.get(id) || this.custom.get(id) || null; }

  /** The entry on air on `id` at `ms`. */
  at(id, ms = Date.now()) {
    const ch = this.get(id);
    if (!ch) return null;
    if (ch.custom) return ch.custom.at(ms);
    return this.engine.entryAt(ch.playlist, ms);
  }

  /** Entries from `fromMs` until `untilMs` (inclusive of the one on air at fromMs). */
  *range(id, fromMs, untilMs) {
    const ch = this.get(id);
    if (!ch) return;
    for (const e of ch.custom ? ch.custom.walk(fromMs) : this.engine.walk(ch.playlist, fromMs)) {
      if (e.startMs >= untilMs) return;
      yield e;
    }
  }

  /** The next `n` shows (videos) after `ms`. */
  nextShows(id, ms, n = 2) {
    const out = [];
    for (const e of this.range(id, ms, ms + 48 * 3600e3)) {
      if (e.startMs >= ms && e.item.type === 'video') out.push(e);
      if (out.length >= n) break;
    }
    return out;
  }
}

/** A display title for any schedule entry. */
export function entryTitle(item) {
  switch (item.type) {
    case 'video': return item.title || item.id;
    case 'ad': return 'Commercial break';
    case 'up-next': return 'Up next';
    case 'bump-break': return 'Station break';
    default: return item.type;
  }
}
