// Turns one schedule entry into an MPEG-1 video + MP2 audio program stream (ffmpeg).
//
// MPEG-1 is chosen for the PS3 app's sake: it can be decoded in software on the PS3's PPU
// with pl_mpeg, so the app needs no proprietary decoder libraries. Every entry type renders
// to the same format so the PS3 just plays one segment after another:
//   video     a show, read straight from archive.org at the right offset
//   ad        a vintage commercial/trailer sliced out of an archive.org "tape"
//   bump-break / up-next   text cards (the site draws these in the browser) over their music
//   weather   the Weather Rewind channel's forecast screens over smooth jazz (weather.js)

import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { weatherPlan, weatherArgs } from './weather.js';

/** A source that sends nothing for this long is given up on (the slot gets a card). */
export const STALL_MS = 12000;
export const OUT = { fps: '30000/1001', vbit: '2800k', abit: '192k', rate: 48000 };

// Loudness: every stream is normalised to the same target (EBU R128, the broadcast standard),
// so channels, shows, commercials and break music all play at a similar level whatever the
// tape's original level was. Single-pass loudnorm works on live streams; it runs at 192 kHz
// internally, hence the resample back. Break music sits a little under the programmes.
export const LOUDNESS = { show: -18, card: -21, truePeak: -1.5, range: 11 };
export const loudnorm = target => `loudnorm=I=${target}:TP=${LOUDNESS.truePeak}:LRA=${LOUDNESS.range},aresample=${OUT.rate}`;
// Frame sizes: widescreen by default, 4:3 for SD sets (the PS3 asks with ?aspect=4x3), and a
// small widescreen one for handhelds (the Vita asks with ?aspect=16x9s).
export const SIZES = { '16x9': { w: 848, h: 480 }, '4x3': { w: 640, h: 480 }, '16x9s': { w: 640, h: 360 } };
const ARCHIVE = 'https://archive.org/download/';
const UA = 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)';

// Card timing, from the site's bump player: each slide holds max(2.5s, 1.5s + 55ms/char),
// the channel ident shows 3.5s first, and a half ends with 1s + 1s of breathing room.
const SLIDE_MIN = 2500, SLIDE_BASE = 1500, SLIDE_PER_CHAR = 55, IDENT = 3500, GAP = 1000, TAIL = 1000, BETWEEN = 300;
const slideMs = text => Math.max(SLIDE_MIN, SLIDE_BASE + text.length * SLIDE_PER_CHAR);

/** How long a station-break half lasts as plan() draws it (relay-only channels schedule with this). */
export function breakHalfMs(half, slides) {
  const body = (slides || []).reduce((t, s) => t + slideMs(s.text), 0) + GAP + TAIL;
  return half === 'open' ? IDENT + body : body;
}

let FONT = process.env.SPELUNKR_FONT || null;
export function font() {
  if (FONT) return FONT;
  for (const q of ['Liberation Sans:bold', 'DejaVu Sans:bold', 'sans:bold']) {
    try {
      const f = execFileSync('fc-match', ['-f', '%{file}', q], { encoding: 'utf8' }).trim();
      if (f && fs.existsSync(f)) return (FONT = f);
    } catch { /* fc-match missing */ }
  }
  throw new Error('no font found for title cards; set SPELUNKR_FONT=/path/to/font.ttf');
}

export const archiveUrl = (id, file) => ARCHIVE + id + '/' + file.split('/').map(encodeURIComponent).join('/');
const audioUrl = (base, filename) => (filename.startsWith(ARCHIVE) ? filename : base + encodeURIComponent(filename));

function wrap(text, width) {
  const lines = [];
  let line = '';
  for (const word of String(text).split(/\s+/).filter(Boolean)) {
    if (line && (line + ' ' + word).length > width) { lines.push(line); line = word; } else line = line ? line + ' ' + word : word;
  }
  if (line) lines.push(line);
  return lines.join('\n');
}

const clock = ms => new Date(ms).toLocaleTimeString('en-US', { hour: 'numeric', minute: '2-digit' });

/**
 * Plans how to render `entry` (from the engine) starting `entry.offsetExact` seconds in.
 * `ctx`: { channel (lineup meta), audioBase, upNext: [entries] }.
 */
export function plan(entry, ctx) {
  const size = SIZES[ctx.aspect] || SIZES['16x9'];
  const wrapAt = Math.round(28 * size.w / 848);
  const it = entry.item;
  const offset = Math.max(0, entry.offsetExact);
  const duration = Math.max(0.5, entry.remaining);

  if (it.type === 'video') {
    const file = it.file || it.mp4;
    return { kind: 'source', size, url: archiveUrl(it.id, file), seek: (it.inS || 0) + offset, duration, crop: it.crop };
  }
  if (it.type === 'weather') return weatherPlan(entry, ctx, size);
  if (it.type === 'ad' && it.slice) {
    const s = it.slice;
    return { kind: 'source', size, url: archiveUrl(s.tape, s.file), seek: (s.inMs || 0) / 1000 + offset, duration, gainDb: s.gainDb };
  }

  const name = (ctx.channel?.name || '').toUpperCase();
  const color = ctx.channel?.color || '#1C0A57';
  const total = entry.durMs ?? it.durationMs ?? (offset + duration) * 1000;
  const texts = [];
  const audio = [];
  let t = 0;
  const push = (text, ms, size = 40) => { texts.push({ text, from: t / 1000, to: (t + ms) / 1000, size }); t += ms; };
  const slides = (bump, ms0) => { for (const s of bump?.slides || []) push(wrap(s.text, wrapAt), slideMs(s.text)); return t - ms0; };

  if (it.type === 'bump-break') {
    if (it.half !== 'close') {
      push(name, IDENT, 52);
      slides(it.channelBump, t);
      t += GAP + TAIL;
      if (it.channelAudio) audio.push({ url: audioUrl(ctx.audioBase, it.channelAudio.filename), seek: it.channelAudio.startOffset || 0, from: 0 });
    }
    if (it.half !== 'open') {
      if (!it.half) t += BETWEEN;
      const from = t / 1000;
      slides(it.platformBump, t);
      if (it.platformAudio) audio.push({ url: audioUrl(ctx.audioBase, it.platformAudio.filename), seek: it.platformAudio.startOffset || 0, from });
    }
  } else if (it.type === 'up-next') {
    const lines = (ctx.upNext || []).map((e, i) => `${i ? 'LATER' : 'NEXT '}  ${clock(e.startMs)}  ${wrap(e.item.title || e.item.id, 30).split('\n')[0]}`);
    texts.push({ text: `UP NEXT ON ${name}`, from: 0, to: total / 1000, size: 34, y: 0.3 });
    texts.push({ text: lines.join('\n\n') || 'MORE SPELUNKR', from: 0, to: total / 1000, size: 28, y: 0.55 });
    if (it.sting) audio.push({ url: audioUrl(ctx.audioBase, it.sting.track), seek: (it.sting.startMs || 0) / 1000, from: 0 });
  } else {
    texts.push({ text: name || 'SPELUNKR', from: 0, to: total / 1000, size: 48 });
  }
  return { kind: 'card', size, color, texts, audio, cardSeconds: total / 1000, seek: offset, duration };
}

/** A card shown when a source can't be played, for whatever time the slot has left. */
export function troublePlan(duration, channel, aspect) {
  return {
    kind: 'card', size: SIZES[aspect] || SIZES['16x9'], color: '#1C0A57', audio: [], cardSeconds: duration, seek: 0, duration,
    texts: [{ text: 'WE ARE EXPERIENCING\nTECHNICAL DIFFICULTIES', from: 0, to: duration, size: 40, y: 0.42 },
      { text: (channel?.name || 'SPELUNKR').toUpperCase() + ' WILL RETURN SHORTLY', from: 0, to: duration, size: 24, y: 0.65 }],
  };
}

export const encodeArgs = () => [
  '-c:v', 'mpeg1video', '-b:v', OUT.vbit, '-maxrate', '4000k', '-bufsize', '1835k', '-g', '15', '-bf', '0',
  '-c:a', 'mp2', '-b:a', OUT.abit, '-ar', String(OUT.rate), '-ac', '2',
  '-max_muxing_queue_size', '2048', '-f', 'mpeg', 'pipe:1',
];

export const httpIn = url => ['-user_agent', UA, '-reconnect', '1', '-reconnect_on_network_error', '1', '-reconnect_delay_max', '4', '-i', url];

/** Builds ffmpeg arguments for a plan. Returns { args, cleanup }. */
export function ffmpegArgs(p) {
  const base = ['-hide_banner', '-loglevel', 'error', '-nostdin'];
  const { w, h } = p.size || SIZES['16x9'];
  const { fps } = OUT;
  const k = w / 848; // card text scales with frame width
  if (p.kind === 'weather') return weatherArgs(p);
  if (p.kind === 'source') {
    const af = ['aresample=async=1:first_pts=0'];
    if (Number.isFinite(p.gainDb) && p.gainDb) af.push(`volume=${p.gainDb}dB`);
    af.push(loudnorm(LOUDNESS.show));
    return {
      args: [...base, '-ss', p.seek.toFixed(3), ...httpIn(p.url), '-t', p.duration.toFixed(3),
        '-map', '0:v:0', '-map', '0:a:0?',
        '-vf', `${p.crop ? `crop=${p.crop},` : ''}scale=${w}:${h}:force_original_aspect_ratio=decrease,pad=${w}:${h}:-1:-1:black,setsar=1,fps=${fps}`,
        '-af', af.join(','), ...encodeArgs()],
      cleanup() {},
    };
  }

  // Title card: coloured field, a band in the channel colour, and timed text slides.
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'spelunkr-card-'));
  const fontfile = font().replace(/[:\\']/g, m => '\\' + m);
  const draws = p.texts.map((t, i) => {
    const file = path.join(dir, `t${i}.txt`);
    fs.writeFileSync(file, t.text);
    const y = t.y != null ? `h*${t.y}-text_h/2` : '(h-text_h)/2';
    return `drawtext=fontfile='${fontfile}':textfile='${file}':fontsize=${Math.round(t.size * k)}:fontcolor=0xe8eaf0:` +
      `line_spacing=10:x=(w-text_w)/2:y=${y}:text_align=C:enable='between(t,${t.from.toFixed(3)},${t.to.toFixed(3)})'`;
  });
  const band = `drawbox=x=0:y=ih-36:w=iw:h=36:color=${p.color}:t=fill`;
  const vchain = `[0:v]${[band, ...draws].join(',')},format=yuv420p[v]`;

  const inputs = ['-f', 'lavfi', '-i', `color=c=0x1C0A57:s=${w}x${h}:r=${fps}:d=${p.cardSeconds.toFixed(3)}`];
  let achain;
  if (p.audio.length) {
    const parts = [];
    p.audio.forEach((a, i) => {
      inputs.push('-ss', a.seek.toFixed(3), ...httpIn(a.url));
      const until = (p.audio[i + 1]?.from ?? p.cardSeconds) - a.from;
      parts.push(`[${i + 1}:a]aresample=${OUT.rate},atrim=0:${until.toFixed(3)},apad=whole_dur=${until.toFixed(3)},asetpts=PTS-STARTPTS[a${i}]`);
    });
    const fade = Math.min(1.5, p.cardSeconds / 4);
    achain = parts.join(';') + ';' + p.audio.map((_, i) => `[a${i}]`).join('') +
      `concat=n=${p.audio.length}:v=0:a=1,${loudnorm(LOUDNESS.card)},afade=t=out:st=${(p.cardSeconds - fade).toFixed(3)}:d=${fade.toFixed(3)}[a]`;
  } else {
    inputs.push('-f', 'lavfi', '-i', `anullsrc=r=${OUT.rate}:cl=stereo`);
    achain = `[1:a]atrim=0:${p.cardSeconds.toFixed(3)}[a]`;
  }
  return {
    args: [...base, ...inputs, '-filter_complex', `${vchain};${achain}`, '-map', '[v]', '-map', '[a]',
      '-ss', p.seek.toFixed(3), '-t', p.duration.toFixed(3), ...encodeArgs()],
    cleanup() { fs.rmSync(dir, { recursive: true, force: true }); },
  };
}

/**
 * Runs a plan, piping the MPEG stream into `out` (a writable). Resolves with
 * { ok, bytes, error }. `signal` aborts (kills ffmpeg).
 */
export function run(p, out, { signal, log = console } = {}) {
  return new Promise(resolve => {
    let cmd;
    try { cmd = ffmpegArgs(p); } catch (e) { resolve({ ok: false, bytes: 0, error: e.message }); return; }
    const ff = spawn('ffmpeg', cmd.args, { stdio: ['ignore', 'pipe', 'pipe'] });
    let bytes = 0, err = '', last = Date.now(), waiting = false, stalled = false;
    const kill = () => ff.kill('SIGKILL');
    signal?.addEventListener('abort', kill, { once: true });
    // A source that stops sending (an overloaded archive.org server can drop to a few KB/s)
    // would otherwise hang the client until it gives up and asks again, over and over.
    // Time spent waiting on the client (backpressure) doesn't count.
    const watchdog = setInterval(() => {
      if (!waiting && Date.now() - last > STALL_MS) { stalled = true; kill(); }
    }, 1000);
    ff.stdout.on('data', chunk => {
      bytes += chunk.length;
      last = Date.now();
      if (!out.write(chunk)) {
        ff.stdout.pause(); waiting = true;
        out.once('drain', () => { waiting = false; last = Date.now(); ff.stdout.resume(); });
      }
    });
    ff.stderr.on('data', d => { err = (err + d).slice(-2000); });
    ff.on('close', code => {
      clearInterval(watchdog);
      signal?.removeEventListener('abort', kill);
      cmd.cleanup();
      if (stalled) err = `source stalled: nothing for ${STALL_MS / 1000}s`;
      if ((code !== 0 || stalled) && !signal?.aborted) log.warn(`[ffmpeg] ${stalled ? err : `exit ${code}: ${err.trim().split('\n').slice(-3).join(' | ')}`}`);
      resolve({ ok: code === 0 && !stalled, bytes, error: err.trim() });
    });
  });
}
