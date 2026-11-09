// Builds src/channels/tvads-9300s.json: US TV commercials that aired 1993-2005, for the
// breaks on the classic MTV channel. Uses archive.org's catalogued off-air captures
// (identifiers like "VHS-005_182050-182952", titled "<Product> Commercial <NETWORK> <date>"),
// which are single commercials rather than whole recorded breaks.
//   node tools/build-tvads.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/tvads-9300s.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const CATALOGUED = /^[A-Z0-9]{3}-[A-Z0-9]{3}_\d+-\d+$/;
const BLOCK = /nsfw|adult|sex|erotic|porn|nude|lingerie|condom|hotline|900|psychic|beer|liquor|vodka|whiskey|cigar|tobacco|lawyer|attorney|injury|political|campaign|vote|candidate/i;
const PER_YEAR = 45, MIN_S = 10, MAX_S = 75;

let s = 7;
const rand = () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32);
async function getJson(url, tries = 4) {
  for (let i = 1; ; i++) {
    try {
      const r = await fetch(url, { headers: UA, signal: AbortSignal.timeout(60000) });
      if (!r.ok) throw new Error(`HTTP ${r.status}`);
      return await r.json();
    } catch (e) { if (i >= tries) throw e; await new Promise(res => setTimeout(res, 2000 * i)); }
  }
}

const picked = [];
for (let year = 1993; year <= 2005; year++) {
  const q = `collection:(vhscommercials OR opensource_movies) AND title:(commercial) AND title:(${year})`;
  const r = await getJson('https://archive.org/advancedsearch.php?q=' + encodeURIComponent(q) + '&fl[]=identifier&fl[]=title&rows=5000&output=json');
  const pool = r.response.docs.filter(d => CATALOGUED.test(d.identifier) && /commercial/i.test(d.title) &&
    !/promo/i.test(d.title) && !BLOCK.test(d.title));
  for (let i = pool.length - 1; i > 0; i--) { const j = Math.floor(rand() * (i + 1)); [pool[i], pool[j]] = [pool[j], pool[i]]; }
  picked.push(...pool.slice(0, PER_YEAR));
  process.stdout.write(`\r${year}: ${pool.length} candidates`);
}

async function ad(doc) {
  const d = await getJson(`https://archive.org/metadata/${doc.identifier}`);
  const v = (d.files || []).filter(f => Number(f.length) > 0 && /\.(mp4|mpe?g|mkv|avi|mov|ts)$/i.test(f.name));
  const f = v.find(x => /\.mp4$/i.test(x.name)) || v[0];
  if (!f || !(Number(f.length) >= MIN_S && Number(f.length) <= MAX_S)) return null;
  const title = doc.title.replace(/\s+Commercial\b.*$/i, '').trim();
  return { id: doc.identifier, file: f.name, durationS: Math.round(Number(f.length) * 10) / 10, title };
}

const out = [];
for (let i = 0; i < picked.length; i += 8) {
  const batch = await Promise.all(picked.slice(i, i + 8).map(d => ad(d).catch(() => null)));
  out.push(...batch.filter(Boolean));
  process.stdout.write(`\r${Math.min(i + 8, picked.length)}/${picked.length} checked, ${out.length} usable   `);
}
out.sort((a, b) => a.id.localeCompare(b.id));
fs.writeFileSync(OUT, JSON.stringify(out, null, 1) + '\n');
console.log(`\n${out.length} ads -> ${path.relative(process.cwd(), OUT)}`);
