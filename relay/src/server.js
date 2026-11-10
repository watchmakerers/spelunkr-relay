// Spelunkr PS3 relay: HTTP server + LAN discovery.
//
//   GET /update/latest, /update/file/<name>   client self-update (see updates.js)
//   GET /ps3/hello            "SPELUNKR <port>": lets clients scanning the LAN recognise a relay
//   GET /ps3/guide?hours=4    channel guide, tab-separated text (format below)
//   GET /ps3/seg/:channel?at=ms[&aspect=4x3]
//                             MPEG-1/MP2 program stream of the entry on air at `at`
//                             (default: now), from that moment to the entry's end
//   GET /watch/:channel       continuous stream, segment after segment (for testing in mpv/VLC)
//   GET /                     status page
//   UDP 47555                 "SPELUNKR?" -> "SPELUNKR <http port>" (the PS3 app finds us this way)
//
// Guide format, one record per line, fields separated by TAB (titles never contain tabs):
//   SPELUNKR  1
//   NOW       <relay clock, ms since epoch>
//   TZ        <relay's local offset from UTC, minutes> (the PS3 shows times in it)
//   CH        <id> <dial> <NAME> <#rrggbb> <description>
//   LOGO      <32 rows of 32 palette letters, concatenated, '.' = transparent> <k=rrggbb,c=rrggbb,...>
//   S         <startMs> <endMs> <title> <year>     a show; spans until the next show begins
//                                                  (station breaks and ads fold into the show before)
//   END

import http from 'node:http';
import dgram from 'node:dgram';
import os from 'node:os';
import { Broadcast } from './broadcast.js';
import { pixelLogo } from './logos.js';
import { plan, troublePlan, run, font } from './render.js';
import { Updates } from './updates.js';
import { startWeather } from './weather.js';

const PORT = Number(process.env.PORT || 8642);
const DISCOVERY_PORT = 47555;
const log = { info: (...a) => console.log(new Date().toISOString(), ...a), warn: (...a) => console.warn(new Date().toISOString(), ...a) };

const bc = new Broadcast({ log });
const updates = new Updates({ log });
const clean = s => String(s ?? '').replace(/[\t\r\n]+/g, ' ').trim();

function shows(id, fromMs, untilMs) {
  // Start well before `fromMs` so the show currently on air (possibly hours long) is included.
  const out = [];
  for (const e of bc.range(id, fromMs - 12 * 3600e3, untilMs)) {
    // a show is a video, or a titled weather segment (the first of each half hour)
    if (e.item.type !== 'video' && !(e.item.type === 'weather' && e.item.title)) continue;
    if (out.length) out[out.length - 1].endMs = e.startMs;
    out.push({ startMs: e.startMs, endMs: e.endMs, title: e.item.title || e.item.id, year: e.item.year || '' });
  }
  return out.filter(s => s.endMs > fromMs);
}

function guide(hours) {
  const now = Date.now();
  const lines = ['SPELUNKR\t1', `NOW\t${now}`, `TZ\t${-new Date(now).getTimezoneOffset()}`];
  for (const ch of bc.list()) {
    if (!bc.get(ch.id)) continue;
    lines.push(['CH', ch.id, ch.dial ?? '', clean(ch.name), ch.color || '#888888', clean(ch.description)].join('\t'));
    const logo = pixelLogo(ch);
    if (logo) lines.push(['LOGO', logo.rows.join(''), Object.entries(logo.palette).map(([k, v]) => `${k}=${v.slice(1)}`).join(',')].join('\t'));
    for (const s of shows(ch.id, now, now + hours * 3600e3)) lines.push(['S', s.startMs, s.endMs, clean(s.title), clean(s.year)].join('\t'));
  }
  lines.push('END', '');
  return lines.join('\n');
}

function context(id, entry, aspect) {
  const ch = bc.get(id);
  return {
    aspect,
    channel: ch?.meta,
    audioBase: bc.lineup?.audioBase || 'https://audio.spelunkr.tv/bumps/',
    upNext: entry.item.type === 'up-next' ? bc.nextShows(id, entry.endMs, 2) : [],
  };
}

/** Streams the entry on air at `atMs` into `res`; on a source failure fills the slot with a card. */
async function streamEntry(id, atMs, res, signal, aspect) {
  const entry = bc.at(id, atMs);
  const ctx = context(id, entry, aspect);
  const p = plan(entry, ctx);
  const t0 = Date.now();
  const r = await run(p, res, { signal, log });
  if (!r.ok && !signal.aborted) {
    // Fill whatever the slot still has. ffmpeg runs faster than real time, so this is
    // measured against the entry, not the wall clock (an error can come mid-stream).
    const left = entry.remaining - Math.min(entry.remaining, (Date.now() - t0) / 1000);
    if (left > 1) await run(troublePlan(left, ctx.channel, aspect), res, { signal, log });
  }
  log.info(`[${id}] ${entry.item.type} "${entry.item.title || ''}" +${entry.offset}s sent ${(r.bytes / 1e6).toFixed(1)}MB in ${((Date.now() - t0) / 1000).toFixed(0)}s`);
  return entry;
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://relay');
  const parts = url.pathname.split('/').filter(Boolean);
  const abort = new AbortController();
  res.on('close', () => abort.abort());

  try {
    if (updates.handle(url, res)) return;

    if (url.pathname === '/ps3/hello') {
      // Lets clients confirm an address really is a relay (used when scanning the LAN).
      res.writeHead(200, { 'content-type': 'text/plain', 'cache-control': 'no-store' });
      return res.end(`SPELUNKR ${PORT}\n`);
    }

    if (url.pathname === '/ps3/guide') {
      const hours = Math.min(24, Math.max(1, Number(url.searchParams.get('hours')) || 4));
      res.writeHead(200, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' });
      return res.end(guide(hours));
    }

    if (parts[0] === 'ps3' && parts[1] === 'seg' && parts[2]) {
      const id = parts[2];
      if (!bc.get(id)) return notFound(res);
      const at = Number(url.searchParams.get('at')) || Date.now();
      const entry = bc.at(id, at);
      res.writeHead(200, {
        'content-type': 'video/mpeg',
        'cache-control': 'no-store',
        'x-seg-start': String(entry.startMs),
        'x-seg-end': String(entry.endMs),
        'x-seg-offset': entry.offsetExact.toFixed(3),
        'x-seg-type': entry.item.type,
        'x-seg-title': encodeURIComponent(entry.item.title || ''),
      });
      await streamEntry(id, at, res, abort.signal, url.searchParams.get('aspect'));
      return res.end();
    }

    if (parts[0] === 'watch' && parts[1]) {
      const id = parts[1];
      if (!bc.get(id)) return notFound(res);
      res.writeHead(200, { 'content-type': 'video/mpeg', 'cache-control': 'no-store' });
      let at = Date.now();
      while (!abort.signal.aborted) {
        const entry = await streamEntry(id, at, res, abort.signal, url.searchParams.get('aspect'));
        at = Math.max(entry.endMs, Date.now());
      }
      return res.end();
    }

    if (url.pathname === '/api/now') {
      const now = Date.now();
      const out = bc.list().filter(c => bc.get(c.id)).map(c => {
        const e = bc.at(c.id, now);
        return { id: c.id, dial: c.dial, name: c.name, type: e.item.type, title: e.item.title || null, offset: e.offset, remaining: Math.round(e.remaining) };
      });
      res.writeHead(200, { 'content-type': 'application/json' });
      return res.end(JSON.stringify({ now, engine: bc.engineInfo, channels: out }, null, 2));
    }

    if (url.pathname === '/') {
      res.writeHead(200, { 'content-type': 'text/html; charset=utf-8' });
      const rows = bc.list().filter(c => bc.get(c.id)).map(c => {
        const e = bc.at(c.id, Date.now());
        const what = e.item.type === 'video' ? esc(e.item.title) : `<i>${esc(e.item.type)}</i>`;
        return `<tr><td>${c.dial ?? ''}</td><td>${esc(c.name)}</td><td>${what}</td><td><a href="/watch/${c.id}">watch</a></td></tr>`;
      }).join('');
      return res.end(`<!doctype html><meta charset=utf-8><title>Spelunkr relay</title>
<style>body{font:14px system-ui;margin:2em;background:#1c0a57;color:#e8eaf0}a{color:#ffd740}td{padding:2px 10px}</style>
<h1>Spelunkr PS3 relay</h1><p>Engine: ${esc(bc.engineInfo.from)} ${esc(bc.engineInfo.build)} · addresses: ${lanAddresses().map(a => `${a}:${PORT}`).join(', ')}</p>
<table>${rows}</table>`);
    }
    notFound(res);
  } catch (e) {
    log.warn(`[http] ${req.url}: ${e.stack || e.message}`);
    if (!res.headersSent) { res.writeHead(500, { 'content-type': 'text/plain' }); res.end(e.message); } else res.end();
  }
});

const esc = s => String(s ?? '').replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
function notFound(res) { res.writeHead(404, { 'content-type': 'text/plain' }); res.end('not found\n'); }

function lanAddresses() {
  return Object.values(os.networkInterfaces()).flat().filter(a => a && a.family === 'IPv4' && !a.internal).map(a => a.address);
}

function startDiscovery() {
  const sock = dgram.createSocket({ type: 'udp4', reuseAddr: true });
  sock.on('message', (msg, rinfo) => {
    if (msg.toString('latin1').startsWith('SPELUNKR?')) sock.send(`SPELUNKR ${PORT}`, rinfo.port, rinfo.address);
  });
  sock.on('error', e => log.warn(`[discovery] ${e.message} (the PS3 app can still connect by address)`));
  sock.bind(DISCOVERY_PORT);
}

font(); // fail early if title cards can't be drawn
await bc.start();
await startWeather(log);
updates.start();
server.listen(PORT, () => {
  log.info(`[relay] ${bc.channels.size} channels · http://${lanAddresses()[0] || 'localhost'}:${PORT}/`);
});
startDiscovery();
