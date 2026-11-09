// Builds src/channels/gameads.json: video game TV commercials from archive.org's
// "videogamecommercials" collection, for the ad breaks on relay-only channels.
//
// Only items titled the collection's catalogue way ("Platform: Game (Year)(Publisher)")
// are used: the collection also holds game cutscenes, demo reels and some adult material,
// and those don't follow that naming. Clips must look like ads (8 s - 2 min).
//   node tools/build-gameads.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/gameads.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const CATALOGUE = /^([^:]{2,40}):\s*(.+?)\s*\((\d{4}|19\dx|20\dx)\)/;
const BLOCK = /nsfw|hentai|adult|playboy|sex|erotic|porn|nude|18\+|xxx|bikini|strip|ecchi|uncensored/i;
const MIN_S = 8, MAX_S = 120;

const res = await fetch('https://archive.org/advancedsearch.php?q=collection%3Avideogamecommercials' +
  '&fl[]=identifier&fl[]=title&fl[]=subject&rows=10000&output=json', { headers: UA });
const docs = (await res.json()).response.docs.filter(d => {
  const t = String(d.title || ''), s = [].concat(d.subject || []).join(' ');
  return CATALOGUE.test(t) && !BLOCK.test(t) && !BLOCK.test(s);
});

async function ad(doc) {
  const d = await (await fetch(`https://archive.org/metadata/${doc.identifier}`, { headers: UA })).json();
  if (BLOCK.test([].concat(d.metadata?.description || '').join(' '))) return null;
  const videos = (d.files || []).filter(f => Number(f.length) > 0 && /\.(mp4|mpe?g|avi|mov|mkv|webm|ogv)$/i.test(f.name));
  const pick = videos.find(f => /\.mp4$/i.test(f.name)) || videos.find(f => f.source === 'original');
  if (!pick) return null;
  const len = Number(pick.length);
  if (!(len >= MIN_S && len <= MAX_S)) return null;
  const m = CATALOGUE.exec(doc.title);
  return { id: doc.identifier, file: pick.name, durationS: Math.round(len * 10) / 10,
    title: `${m[2]} (${m[1]}, ${m[3]})` };
}

const out = [];
for (let i = 0; i < docs.length; i += 8) {
  const batch = await Promise.all(docs.slice(i, i + 8).map(d => ad(d).catch(() => null)));
  out.push(...batch.filter(Boolean));
  process.stdout.write(`\r${Math.min(i + 8, docs.length)}/${docs.length} checked, ${out.length} usable`);
}
out.sort((a, b) => a.id.localeCompare(b.id));
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify(out, null, 1) + '\n');
console.log(`\n${out.length} ads -> ${path.relative(process.cwd(), OUT)}`);
