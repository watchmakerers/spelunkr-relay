// Builds src/channels/toons.json: episodes for the relay's 80s cartoon channel, from
// archive.org uploads of each show (English versions; reboots, movies and foreign dubs
// were ruled out by hand). Episode titles are cleaned up from the file names.
//   node tools/build-toons.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/toons.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };

// show, its archive.org items, the year it premiered, and the episode length window (min).
const SHOWS = [
  { show: 'He-Man', year: '1983', items: ['he-man-and-the-masters-of-the-universe-s01', 'he-man-and-the-masters-of-the-universe-s02'], strip: /he-man and the masters of the universe/i },
  { show: 'ThunderCats', year: '1985', items: ['thundercats_20250306'] },
  { show: 'Teenage Mutant Ninja Turtles', year: '1987', items: ['tmnt-season-3-2', 'tmnt-season-9-10'] },
  { show: 'G.I. Joe', year: '1985', items: ['g.i.-joe-a-real-american-hero-sunbow-dvd-rips'] },
  { show: 'The Transformers', year: '1984', items: ['The_Transformers_DVDISO'], min: 15, untitled: true },
  { show: 'Super Mario Bros. Super Show', year: '1989', items: ['the-super-mario-bros.-super-show-select-episodes', 'thesupermariobrossupershow'], strip: /the super mario bros\.? super show!?/i },
  { show: 'Dungeons & Dragons', year: '1983', items: ['dungeons-dragons-tv-series'], untitled: true },
  { show: 'Voltron', year: '1984', items: ['voltron-the-defender-of-the-universe-vehicle-force-1984'], strip: /voltron vehicle force/i },
  { show: "Hulk Hogan's Rock 'n' Wrestling", year: '1985', items: ['hulk-hogans-rock-n-wrestling'] },
  { show: 'The Legend of Zelda', year: '1989', items: ['the-legend-of-zelda-1989-full-series'], min: 9, strip: /the legend of zelda \(1989\)/i },
  { show: 'Star Wars Droids', year: '1985', items: ['10starwarsdroids1x09.5thegreatheep'], strip: /star wars\s+droids/i },
  { show: 'Star Wars Ewoks', year: '1985', items: ['113starwarsewoks1x13asha', '28starwarsewoks2x08theseasonscepter'], min: 9, strip: /star wars\s+ewoks/i },
  { show: 'DuckTales', year: '1987', items: ['ducktales-1987_202601'] },
  { show: 'TaleSpin', year: '1990', items: ['tale-spin-1990-complete-series'] },
  { show: 'Darkwing Duck', year: '1991', items: ['darkwing-duck-1991_202601'] },
  { show: 'Gargoyles', year: '1994', items: ['gargoyles-1080p-ai-upscale'] },
  { show: 'BraveStarr', year: '1987', items: ['bravestarr-bci-dvd'], untitled: true },
  { show: 'Thundarr the Barbarian', year: '1980', items: ['Thundarr_the_Barbarian'] },
  { show: 'Challenge of the GoBots', year: '1984', items: ['challenge-of-the-gobots'] },
];

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

/** Episode name from a file name: drop folders, extension, show name, episode codes, rip tags. */
function episodeName(file, spec) {
  let s = path.basename(file).replace(/(\.ia)?\.(mp4|m4v)$/i, '');
  if (spec.strip) s = s.replace(spec.strip, ' ');
  s = s.replace(new RegExp(spec.show.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'ig'), ' ')
    .replace(/\((?:\d{3,4}p[^)]*|dvdrip[^)]*|pilot[^)]*)\)/ig, m => (/pilot/i.test(m) ? m : ' '))
    .replace(/\bS\d+\s*E\d+\b|\b\d+x\d+(\.\d+)?[a-z]?\b|\bEp\.?\s*\d+\b|\bEP\s*\d+\b/ig, ' ')
    .replace(/\b(dvdrip|xvid|x264|480p|720p|1080p|upscale)\b/ig, ' ')
    .replace(/^[\s\d._-]+(?:[ab]\b)?|[\s._-]+$/g, '')
    .replace(/[_]+/g, ' ')
    .replace(/\s+-\s+/g, ' - ').replace(/^-\s*|\s*-$/g, '')
    .replace(/\s{2,}/g, ' ').trim();
  return s.replace(/^[-\s]+/, '').replace(/^['"]|['"]$/g, '').trim();
}

const episodes = [];
for (const spec of SHOWS) {
  const seen = new Set();
  let n = 0;
  for (const item of spec.items) {
    const d = await getJson(`https://archive.org/metadata/${item}`);
    const files = (d.files || []).filter(f => /\.(mp4|m4v)$/i.test(f.name) && !/\.ia\.mp4$/i.test(f.name));
    const lo = (spec.min ?? 15) * 60, hi = (spec.max ?? 35) * 60;
    for (const f of files.sort((a, b) => a.name.localeCompare(b.name, 'en', { numeric: true }))) {
      const len = Number(f.length);
      if (!(len >= lo && len <= hi)) continue;
      const name = spec.untitled ? '' : episodeName(f.name, spec);
      const key = name.toLowerCase().replace(/[^a-z0-9]/g, '');
      if (key && seen.has(key)) continue; // the same episode in a second upload
      if (key) seen.add(key);
      n++;
      episodes.push({
        show: spec.show, id: item, file: f.name, durationS: Math.round(len * 10) / 10, year: spec.year,
        title: name ? `${spec.show}: ${name}` : `${spec.show} #${n}`,
        width: Number(f.width) || null, height: Number(f.height) || null,
      });
    }
  }
  console.log(`${spec.show}: ${n} episodes`);
}

fs.writeFileSync(OUT, JSON.stringify(episodes, null, 1) + '\n');
console.log(`${episodes.length} episodes, ${(episodes.reduce((t, e) => t + e.durationS, 0) / 3600).toFixed(0)} hours -> ${path.relative(process.cwd(), OUT)}`);
