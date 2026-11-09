// Builds src/channels/mtv.json: episodes for the relay's classic MTV channel, from
// archive.org uploads of Jackass, Beavis and Butt-Head, Celebrity Deathmatch, Viva La Bam
// and Headbangers Ball. Each entry is one playable file (the h.264 MP4 where archive.org
// made one). Multi-part broadcasts share a `group` so the scheduler keeps them in order.
//   node tools/build-mtv.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/mtv.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const HEADBANGERS_MAX = 14; // broadcasts; each is 1-3 hours, so this keeps the mix balanced
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

async function getJson(url, tries = 4) {
  for (let i = 1; ; i++) {
    try {
      const r = await fetch(url, { headers: UA, signal: AbortSignal.timeout(60000) });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return await r.json();
    } catch (e) {
      if (i >= tries) throw e;
      await new Promise(res => setTimeout(res, 2000 * i));
    }
  }
}
const meta = id => getJson(`https://archive.org/metadata/${id}`);
const titleCase = s => s.replace(/[._]+/g, ' ').replace(/\s+/g, ' ').trim().replace(/\b\w/g, c => c.toUpperCase());

/** Playable video files of an item: MP4s (originals or h.264 derivatives), one per episode. */
function videos(d) {
  const fs = (d.files || []).filter(f => Number(f.length) > 0);
  const mp4 = fs.filter(f => /\.(mp4|m4v)$/i.test(f.name));
  const byStem = new Map();
  for (const f of mp4) {
    const stem = f.name.replace(/(\.ia)?\.(mp4|m4v)$/i, '');
    const cur = byStem.get(stem);
    // prefer the plain h.264 derivative, then an original MP4, then the ".ia" re-encode
    const rank = x => (/\.ia\.mp4$/i.test(x.name) ? 2 : x.format === 'h.264' ? 0 : 1);
    if (!cur || rank(f) < rank(cur)) byStem.set(stem, f);
  }
  return [...byStem.values()].sort((a, b) => a.name.localeCompare(b.name, 'en', { numeric: true }));
}

const episodes = [];
const add = (show, id, f, title, extra = {}) => episodes.push({
  show, id, file: f.name, durationS: Math.round(Number(f.length) * 10) / 10, title, year: '', ...extra });

// --- Jackass (2000-2002)
{
  const d = await meta('jackasscompleteseries');
  for (const f of videos(d)) {
    const m = /S(\d+)E(\d+)\s*(.*)\.mp4$/i.exec(path.basename(f.name));
    add('Jackass', d.metadata.identifier, f, m ? `Jackass: ${m[3].trim()}` : 'Jackass', { year: '2000' });
  }
}
// --- Beavis and Butt-Head (seasons 2-8)
for (const item of ['s-2-ep-13-no-laughing', 's-7-ep-41-beavis-butthead-are-dead']) {
  const d = await meta(item);
  for (const f of videos(d)) {
    if (Number(f.length) < 240) continue; // stray music-video clips
    const m = /EP\s*\d+\s*(.*?)(\.ia)?\.(mp4|m4v)$/i.exec(path.basename(f.name));
    add('Beavis and Butt-Head', item, f, `Beavis and Butt-Head: ${m ? m[1].trim() : path.basename(f.name, '.mp4')}`, { year: '1993' });
  }
}
// --- Celebrity Deathmatch (1998-2002)
{
  const d = await meta('celebrity_deathmatch_season1');
  for (const f of videos(d)) {
    const m = /S(\d+)E(\d+)/i.exec(f.name);
    add('Celebrity Deathmatch', 'celebrity_deathmatch_season1', f,
      m ? `Celebrity Deathmatch: Season ${Number(m[1])}, Ep. ${Number(m[2])}` : 'Celebrity Deathmatch', { year: '1998' });
  }
}
// --- Viva La Bam (2003-2005)
{
  const d = await meta('vivalabam');
  for (const f of videos(d)) {
    const m = /s(\d+)e(\d+)((?:\.(?!dvdrip|xvid|tvrip|hdtv)[^.]+)*)/i.exec(path.basename(f.name));
    const name = m && m[3] ? titleCase(m[3]) : m ? `Season ${Number(m[1])}, Ep. ${Number(m[2])}` : titleCase(path.basename(f.name, '.mp4'));
    add('Viva La Bam', 'vivalabam', f, `Viva La Bam: ${name}`, { year: '2003' });
  }
}
// --- Headbangers Ball (1987-1995 broadcasts)
{
  const s = await getJson('https://archive.org/advancedsearch.php?q=' + encodeURIComponent('title:("headbangers ball") AND mediatype:movies') +
    '&fl[]=identifier&fl[]=title&fl[]=downloads&sort[]=downloads+desc&rows=300&output=json');
  let used = 0;
  for (const doc of s.response.docs) {
    if (used >= HEADBANGERS_MAX) break;
    let d;
    try { d = await meta(doc.identifier); } catch { continue; }
    const parts = videos(d).filter(f => Number(f.length) >= 8 * 60 && !/VIDEO_TS\.(VOB|mp4)$/i.test(f.name));
    const total = parts.reduce((t, f) => t + Number(f.length), 0);
    if (!parts.length || total < 40 * 60 || total > 4 * 3600) continue;
    const date = /(19\d\d)[.\-](\d\d)[.\-](\d\d)/.exec(doc.identifier + ' ' + doc.title) ||
      /(\d\d?)-(\d\d?)-(\d\d)(?!\d)/.exec(doc.identifier);
    let when = '', year = '';
    if (date && date[1].length === 4) { when = `${MONTHS[date[2] - 1]} ${Number(date[3])}, ${date[1]}`; year = date[1]; }
    else if (date) { year = `19${date[3]}`; when = `${MONTHS[date[1] - 1]} ${Number(date[2])}, ${year}`; }
    parts.forEach((f, i) => add('Headbangers Ball', doc.identifier, f,
      `Headbangers Ball${when ? ` (${when})` : ''}${parts.length > 1 ? ` pt. ${i + 1}` : ''}`,
      { year, group: doc.identifier, part: i }));
    used++;
    process.stdout.write(`\rHeadbangers Ball broadcasts: ${used}`);
  }
}

fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify(episodes, null, 1) + '\n');
const by = {};
for (const e of episodes) { by[e.show] ??= { n: 0, h: 0 }; by[e.show].n++; by[e.show].h += e.durationS / 3600; }
console.log('\n' + Object.entries(by).map(([k, v]) => `${k}: ${v.n} files, ${v.h.toFixed(1)} h`).join('\n'));
