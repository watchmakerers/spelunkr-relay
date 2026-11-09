// Builds src/channels/xplay.json: every full X-Play episode on archive.org with its MP4
// and running time, so the relay can schedule the channel without hitting archive.org
// at startup. Re-run to pick up newly uploaded episodes:  node tools/build-xplay.js
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const OUT = path.join(path.dirname(fileURLToPath(import.meta.url)), '../src/channels/xplay.json');
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };
const MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

const search = await fetch('https://archive.org/advancedsearch.php?q=' +
  encodeURIComponent('identifier:X-Play.* AND mediatype:movies') + '&fl[]=identifier&rows=5000&output=json', { headers: UA });
const ids = (await search.json()).response.docs.map(d => d.identifier).sort();

async function episode(id) {
  const r = await fetch(`https://archive.org/metadata/${id}`, { headers: UA });
  const d = await r.json();
  const mp4 = (d.files || []).find(f => /\.mp4$/i.test(f.name) && Number(f.length) > 60);
  if (!mp4) return null;
  const m = /(\d{4})\.(\d{2})\.(\d{2})/.exec(id);
  const desc = [].concat(d.metadata?.description || '').join(' ').replace(/<[^>]+>/g, ' ').replace(/\s+/g, ' ').trim();
  let title = (d.metadata?.title || id).replace(/^X-Play\s*/i, '').trim();
  if (m) title = `${MONTHS[Number(m[2]) - 1]} ${Number(m[3])}, ${m[1]}`;
  return {
    id, file: mp4.name, durationS: Math.round(Number(mp4.length) * 10) / 10,
    width: Number(mp4.width) || null, height: Number(mp4.height) || null,
    title: `X-Play: ${title}`, year: m ? m[1] : '', description: desc,
  };
}

const out = [];
for (let i = 0; i < ids.length; i += 8) {
  const batch = await Promise.all(ids.slice(i, i + 8).map(id => episode(id).catch(e => (console.warn(id, e.message), null))));
  out.push(...batch.filter(Boolean));
  process.stdout.write(`\r${Math.min(i + 8, ids.length)}/${ids.length}`);
}
fs.mkdirSync(path.dirname(OUT), { recursive: true });
fs.writeFileSync(OUT, JSON.stringify(out, null, 1) + '\n');
const hours = out.reduce((s, e) => s + e.durationS, 0) / 3600;
console.log(`\n${out.length} episodes, ${hours.toFixed(1)} hours -> ${path.relative(process.cwd(), OUT)}`);
