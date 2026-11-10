// Weather Rewind: a relay-only channel that runs the local forecast the way cable weather
// did in the early 90s. Blue screens with gold trim, a ticking clock, a crawl of current
// conditions along the bottom and smooth jazz underneath, broken up by 90s commercials.
//
// Forecasts are live (Open-Meteo: free, no key) for the relay's location:
//   WEATHER_LOCATION   "Portland, OR" or "45.52,-122.68" (or the first line of relay/weather.txt);
//                      without it the location is guessed from the relay's public IP
//   WEATHER_UNITS      "imperial" (default: °F, mph, inHg) or "metric" (°C, km/h, hPa)
//
// The screens are drawn as SVG, which ffmpeg rasterises (librsvg); the clock and the crawl
// are drawn live by ffmpeg on top, so the clock ticks in the stream.
//
// Schedule: every half hour (on the half hour) is four forecast segments, each followed by
// a commercial break. Music and ads come from fixed shuffles indexed by time, so every
// relay plays the same song and the same ads at the same moment, like the other channels.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { OUT, SIZES, font, encodeArgs, httpIn, loudnorm, LOUDNESS, archiveUrl } from './render.js';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const read = f => JSON.parse(fs.readFileSync(path.join(HERE, f), 'utf8'));
const UA = { 'user-agent': 'spelunkr-ps3-relay/0.1 (+https://spelunkr.tv)' };

// ---------------------------------------------------------------------------------------
// Schedule

const ANCHOR_MS = Date.UTC(2026, 0, 1); // on a half hour in every whole- and half-hour time zone
const BLOCK_MS = 30 * 60e3;
const SEGMENTS = 4; // forecast segments per half hour
const ADS_PER_BREAK = [3, 2, 3, 2];
const ADS_PER_BLOCK = ADS_PER_BREAK.reduce((a, b) => a + b, 0);
const TRACKS_PER_SEGMENT = 3; // music pointer step per segment (a segment uses 1-3 songs)

function rng(seed) {
  let s = seed >>> 0 || 1;
  return () => ((s = (s * 1664525 + 1013904223) >>> 0) / 2 ** 32);
}
function shuffle(list, seed) {
  const rand = rng(seed), a = list.slice();
  for (let i = a.length - 1; i > 0; i--) {
    const j = Math.floor(rand() * (i + 1));
    [a[i], a[j]] = [a[j], a[i]];
  }
  return a;
}

// A sun peeking out from behind a cloud, in the site's pixel-logo letters.
const WEATHER_LOGO = (() => {
  const g = Array.from({ length: 32 }, () => Array(32).fill('.'));
  for (let y = 2; y < 30; y++) for (let x = 2; x < 30; x++) g[y][x] = (x <= 3 || x >= 28 || y <= 3 || y >= 28) ? 'k' : 'c';
  const sun = [12, 12, 6.5];
  for (let y = 4; y < 28; y++) for (let x = 4; x < 28; x++) {
    const d = Math.hypot(x - sun[0], y - sun[1]);
    if (d <= sun[2]) g[y][x] = d > sun[2] - 1.5 ? 'o' : 'g';
    else if (d <= 10.5 && d >= 8.5 && (Math.round(Math.atan2(y - sun[1], x - sun[0]) / (Math.PI / 4) * 2) % 2 === 0)) g[y][x] = 'g';
  }
  const puffs = [[15, 21, 4.5], [20, 18, 5.5], [25, 21, 3.5]];
  for (let y = 4; y < 28; y++) for (let x = 4; x < 28; x++) {
    const inPuff = puffs.some(([cx, cy, r]) => Math.hypot(x - cx, y - cy) <= r) || (y >= 21 && y <= 25 && x >= 12 && x <= 27);
    if (inPuff) g[y][x] = y >= 24 ? 's' : 'p';
  }
  return { type: 'pixel', rows: g.map(r => r.join('')) };
})();

export class WeatherChannel {
  constructor() {
    this.meta = {
      id: 'relay-weather', dial: 7, name: 'Weather Rewind', color: '#2a5bd7',
      description: 'Your local forecast the way cable did it in the early 90s: blue screens, a ticking clock and smooth jazz, with 90s commercials.',
      logomark: WEATHER_LOGO,
    };
    const year = a => Number((a.file.match(/_((?:19|20)\d\d)-\d\d-\d\d/) || [])[1]);
    this.ads = shuffle(read('channels/tvads-9300s.json').filter(a => year(a) >= 1993 && year(a) <= 1999), 1990);
    this.music = shuffle(read('channels/weather-music.json'), 1986);
    if (!this.ads.length || !this.music.length) throw new Error('no ads or music');
    this.blocks = new Map();
  }

  /** The entries of half-hour block `k` (block 0 starts at ANCHOR_MS). */
  block(k) {
    if (this.blocks.has(k)) return this.blocks.get(k);
    const startMs = ANCHOR_MS + k * BLOCK_MS;
    const n = this.ads.length, base = ((k * ADS_PER_BLOCK) % n + n) % n;
    const breaks = [];
    let a = 0;
    for (const count of ADS_PER_BREAK) {
      breaks.push(Array.from({ length: count }, () => this.ads[(base + a++) % n]));
    }
    const adMs = breaks.flat().reduce((t, ad) => t + Math.round(ad.durationS * 1000), 0);
    const segMs = Math.floor((BLOCK_MS - adMs) / SEGMENTS);
    const items = [];
    breaks.forEach((ads, i) => {
      const len = i === SEGMENTS - 1 ? BLOCK_MS - adMs - segMs * (SEGMENTS - 1) : segMs;
      items.push({
        type: 'weather', durMs: len, segment: k * SEGMENTS + i,
        title: i === 0 ? blockTitle(startMs) : undefined,
        music: this.songs(k * SEGMENTS + i, len),
      });
      for (const ad of ads) {
        items.push({ type: 'ad', title: ad.title, durMs: Math.round(ad.durationS * 1000), slice: { tape: ad.id, file: ad.file, inMs: Math.round((ad.inS || 0) * 1000), lengthMs: Math.round(ad.durationS * 1000) } });
      }
    });
    let t = startMs;
    for (const it of items) { it.startMs = t; t += it.durMs; }
    const out = { k, startMs, items };
    this.blocks.set(k, out);
    if (this.blocks.size > 8) this.blocks.delete(this.blocks.keys().next().value);
    return out;
  }

  /** Songs filling a segment of `ms`, starting from that segment's place in the shuffle. */
  songs(segment, ms) {
    const n = this.music.length, out = [];
    let i = ((segment * TRACKS_PER_SEGMENT) % n + n) % n, t = 0;
    while (t < ms) {
      const s = this.music[i++ % n];
      out.push({ id: s.id, file: s.file, durationS: s.durationS, fromS: t / 1000 });
      t += s.durationS * 1000;
    }
    return out;
  }

  entry(it, ms) {
    const endMs = it.startMs + it.durMs;
    const offsetExact = Math.max(0, (ms - it.startMs) / 1000);
    return { item: it, startMs: it.startMs, endMs, durMs: it.durMs, offsetExact, offset: Math.floor(offsetExact), remaining: (endMs - Math.max(ms, it.startMs)) / 1000 };
  }

  at(ms) {
    const b = this.block(Math.floor((ms - ANCHOR_MS) / BLOCK_MS));
    let it = b.items[0];
    for (const x of b.items) if (x.startMs <= ms) it = x;
    return this.entry(it, ms);
  }

  *walk(fromMs) {
    let k = Math.floor((fromMs - ANCHOR_MS) / BLOCK_MS);
    for (;;) {
      for (const it of this.block(k).items) if (it.startMs + it.durMs > fromMs) yield this.entry(it, fromMs);
      k++;
    }
  }
}

function blockTitle(ms) {
  const h = new Date(ms).getHours();
  if (h < 5) return 'Weather Overnight';
  if (h < 11) return 'Your Morning Forecast';
  if (h < 17) return 'Afternoon Outlook';
  return 'Evening Forecast';
}

// ---------------------------------------------------------------------------------------
// Live weather

const METRIC = String(process.env.WEATHER_UNITS || '').toLowerCase() === 'metric';
const U = METRIC
  ? { temp: 'C', speed: 'km/h', dist: 'km', pressure: 'hPa', q: 'temperature_unit=celsius&wind_speed_unit=kmh' }
  : { temp: 'F', speed: 'mph', dist: 'mi', pressure: 'in', q: 'temperature_unit=fahrenheit&wind_speed_unit=mph' };
const REFRESH_MS = 10 * 60e3;
let wx = null; // { place, data, fetchedAt }

async function getJson(url) {
  const r = await fetch(url, { headers: UA, signal: AbortSignal.timeout(20000) });
  if (!r.ok) throw new Error(`${new URL(url).host}: HTTP ${r.status}`);
  return r.json();
}

async function locate() {
  let want = process.env.WEATHER_LOCATION;
  if (!want) {
    try { want = fs.readFileSync(path.join(HERE, '../weather.txt'), 'utf8').split('\n')[0].trim(); } catch { /* none */ }
  }
  const ll = want && want.match(/^\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)\s*$/);
  if (ll) return { name: 'Your Area', lat: +ll[1], lon: +ll[2] };
  if (want) {
    // "Portland, OR": search the city, then prefer a match whose region/country fits the rest
    const [city, ...rest] = want.split(',').map(s => s.trim());
    const d = await getJson(`https://geocoding-api.open-meteo.com/v1/search?name=${encodeURIComponent(city)}&count=10&language=en`);
    const hint = rest.join(' ').toLowerCase();
    const fits = r => !hint || [r.admin1, r.country, r.country_code, stateCode(r.admin1)].some(x => x && x.toLowerCase().includes(hint) || (x && hint.includes(x.toLowerCase())));
    const r = (d.results || []).find(fits) || (d.results || [])[0];
    if (!r) throw new Error(`can't find "${want}"`);
    return { name: r.name, lat: r.latitude, lon: r.longitude };
  }
  const ip = await getJson('https://ipapi.co/json/');
  if (!Number.isFinite(ip.latitude)) throw new Error('could not guess the location from this IP; set WEATHER_LOCATION');
  return { name: ip.city || 'Your Area', lat: ip.latitude, lon: ip.longitude };
}

const STATES = { Alabama: 'AL', Alaska: 'AK', Arizona: 'AZ', Arkansas: 'AR', California: 'CA', Colorado: 'CO', Connecticut: 'CT', Delaware: 'DE', Florida: 'FL', Georgia: 'GA', Hawaii: 'HI', Idaho: 'ID', Illinois: 'IL', Indiana: 'IN', Iowa: 'IA', Kansas: 'KS', Kentucky: 'KY', Louisiana: 'LA', Maine: 'ME', Maryland: 'MD', Massachusetts: 'MA', Michigan: 'MI', Minnesota: 'MN', Mississippi: 'MS', Missouri: 'MO', Montana: 'MT', Nebraska: 'NE', Nevada: 'NV', 'New Hampshire': 'NH', 'New Jersey': 'NJ', 'New Mexico': 'NM', 'New York': 'NY', 'North Carolina': 'NC', 'North Dakota': 'ND', Ohio: 'OH', Oklahoma: 'OK', Oregon: 'OR', Pennsylvania: 'PA', 'Rhode Island': 'RI', 'South Carolina': 'SC', 'South Dakota': 'SD', Tennessee: 'TN', Texas: 'TX', Utah: 'UT', Vermont: 'VT', Virginia: 'VA', Washington: 'WA', 'West Virginia': 'WV', Wisconsin: 'WI', Wyoming: 'WY' };
const stateCode = s => STATES[s] || null;

async function fetchWeather(place) {
  const q = [
    `latitude=${place.lat}`, `longitude=${place.lon}`, 'timezone=auto', 'forecast_days=7', 'past_hours=3', U.q,
    'current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m,dew_point_2m,visibility,is_day',
    'hourly=temperature_2m,weather_code,precipitation_probability,wind_speed_10m,wind_direction_10m,pressure_msl',
    'daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset',
  ].join('&');
  return getJson(`https://api.open-meteo.com/v1/forecast?${q}`);
}

/** Finds the location and keeps the forecast fresh. Never throws; the channel shows a notice until data arrives. */
export async function startWeather(log = console) {
  let place = null;
  const tick = async () => {
    try {
      if (!place) { place = await locate(); log.info(`[weather] forecasting for ${place.name} (${place.lat.toFixed(2)}, ${place.lon.toFixed(2)})`); }
      wx = { place, data: await fetchWeather(place), fetchedAt: Date.now() };
    } catch (e) {
      log.warn(`[weather] ${e.message}`);
    }
  };
  await tick();
  setInterval(tick, REFRESH_MS).unref();
}

// ---------------------------------------------------------------------------------------
// Words

const CODES = {
  0: ['Sunny', 'Clear', 'sun'], 1: ['Mostly Sunny', 'Mostly Clear', 'sun'], 2: ['Partly Cloudy', 'Partly Cloudy', 'partly'], 3: ['Cloudy', 'Cloudy', 'cloud'],
  45: ['Fog', 'Fog', 'fog'], 48: ['Freezing Fog', 'Freezing Fog', 'fog'],
  51: ['Light Drizzle', 'Light Drizzle', 'rain'], 53: ['Drizzle', 'Drizzle', 'rain'], 55: ['Heavy Drizzle', 'Heavy Drizzle', 'rain'],
  56: ['Freezing Drizzle', 'Freezing Drizzle', 'sleet'], 57: ['Freezing Drizzle', 'Freezing Drizzle', 'sleet'],
  61: ['Light Rain', 'Light Rain', 'rain'], 63: ['Rain', 'Rain', 'rain'], 65: ['Heavy Rain', 'Heavy Rain', 'rain'],
  66: ['Freezing Rain', 'Freezing Rain', 'sleet'], 67: ['Freezing Rain', 'Freezing Rain', 'sleet'],
  71: ['Light Snow', 'Light Snow', 'snow'], 73: ['Snow', 'Snow', 'snow'], 75: ['Heavy Snow', 'Heavy Snow', 'snow'], 77: ['Snow Grains', 'Snow Grains', 'snow'],
  80: ['Showers', 'Showers', 'rain'], 81: ['Rain Showers', 'Rain Showers', 'rain'], 82: ['Heavy Showers', 'Heavy Showers', 'rain'],
  85: ['Snow Showers', 'Snow Showers', 'snow'], 86: ['Snow Showers', 'Snow Showers', 'snow'],
  95: ['T\'Storms', 'T\'Storms', 'storm'], 96: ['T\'Storms', 'T\'Storms', 'storm'], 99: ['T\'Storms', 'T\'Storms', 'storm'],
};
const cond = (code, day = true) => (CODES[code] || ['Unknown', 'Unknown', 'cloud'])[day ? 0 : 1];
const iconOf = (code, day = true) => {
  const k = (CODES[code] || [0, 0, 'cloud'])[2];
  return day ? k : k === 'sun' ? 'moon' : k === 'partly' ? 'partlyNight' : k;
};
const COMPASS = ['N', 'NNE', 'NE', 'ENE', 'E', 'ESE', 'SE', 'SSE', 'S', 'SSW', 'SW', 'WSW', 'W', 'WNW', 'NW', 'NNW'];
const WORDS = { N: 'north', NE: 'northeast', E: 'east', SE: 'southeast', S: 'south', SW: 'southwest', W: 'west', NW: 'northwest' };
const dir16 = deg => COMPASS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16];
const dir8 = deg => ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(((deg % 360) + 360) % 360 / 45) % 8];
const r0 = v => (Number.isFinite(v) ? Math.round(v) : '--');
const pressure = hpa => (METRIC ? `${Math.round(hpa)}` : (hpa * 0.02953).toFixed(2));
const visibility = m => (METRIC ? `${Math.round(m / 1000)}` : `${Math.min(10, Math.round(m / 1609.34))}`);
const DAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday'];
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const weekday = isoDate => new Date(`${isoDate}T12:00:00Z`).getUTCDay();
const hhmm = iso => {
  const [h, m] = iso.slice(11, 16).split(':').map(Number);
  return `${((h + 11) % 12) + 1}:${String(m).padStart(2, '0')} ${h < 12 ? 'am' : 'pm'}`;
};

/** Local date/hour at the forecast location for `ms`. */
function localParts(ms, tz) {
  const p = Object.fromEntries(new Intl.DateTimeFormat('en-CA', { timeZone: tz, year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', hourCycle: 'h23' })
    .formatToParts(new Date(ms)).map(x => [x.type, x.value]));
  return { date: `${p.year}-${p.month}-${p.day}`, hour: Number(p.hour) };
}

/** Day and night forecast periods ("TONIGHT", "TUESDAY", ...) with a sentence each. */
function periods(d, ms) {
  const h = d.hourly, { date, hour } = localParts(ms, d.timezone);
  const dayIx = d.daily.time.indexOf(date);
  const out = [];
  // a period is the hours [from, to) of a local day: day 06-18, night 18-06 next morning
  const windowOf = (iso, night) => {
    const start = `${iso}T${night ? '18' : '06'}:00`;
    const i0 = h.time.indexOf(start);
    return i0 < 0 ? null : { i0, i1: Math.min(h.time.length, i0 + 12) };
  };
  const startNight = hour >= 15;
  for (let n = 0; n < 4; n++) {
    const half = n + (startNight ? 1 : 0); // 0 = today, 1 = tonight, 2 = tomorrow ...
    const di = dayIx + Math.floor(half / 2), night = half % 2 === 1;
    const iso = d.daily.time[di];
    if (!iso) break;
    const w = windowOf(iso, night);
    if (!w) continue;
    const rng = (k) => h[k].slice(w.i0, w.i1).filter(Number.isFinite);
    const temps = rng('temperature_2m'), winds = rng('wind_speed_10m'), pops = rng('precipitation_probability');
    const codes = h.weather_code.slice(w.i0, w.i1);
    const code = dominant(codes);
    const name = half === 0 ? 'Today' : half === 1 ? 'Tonight' : `${DAYS[weekday(iso)]}${night ? ' Night' : ''}`;
    const dirs = h.wind_direction_10m.slice(w.i0, w.i1);
    const mean = dirs.length ? circularMean(dirs) : 0;
    let s = `${sky(code, !night)}. ${night ? 'Low' : 'High'} ${night ? r0(Math.min(...temps)) : r0(Math.max(...temps))}. `;
    const lo = Math.round(Math.min(...winds) / 5) * 5, hi = Math.round(Math.max(...winds) / 5) * 5;
    s += hi < 5 ? 'Winds light and variable.' : `${cap(WORDS[dir8(mean)])} wind ${lo && lo < hi ? `${lo} to ${hi}` : `around ${hi}`} ${U.speed}.`;
    const pop = Math.round(Math.max(0, ...pops) / 10) * 10;
    if (pop >= 20 && /rain|drizzle|shower|snow|storm|sleet|freezing/i.test(cond(code))) s += ` Chance of ${precipWord(code)} ${pop} percent.`;
    else if (pop >= 30) s += ` Chance of precipitation ${pop} percent.`;
    out.push({ name, text: s });
  }
  return out;
}
const cap = s => s.charAt(0).toUpperCase() + s.slice(1);
function dominant(codes) {
  // the most severe weather that lasts at least two hours, else the most common sky
  const count = new Map();
  for (const c of codes) count.set(c, (count.get(c) || 0) + 1);
  const wet = [...count].filter(([c, n]) => c >= 45 && n >= 2).sort((a, b) => b[0] - a[0]);
  if (wet.length) return wet[0][0];
  return [...count].sort((a, b) => b[1] - a[1] || b[0] - a[0])[0]?.[0] ?? 3;
}
function sky(code, day) {
  if (code <= 3) return [day ? 'Sunny' : 'Clear', day ? 'Mostly sunny' : 'Mostly clear', 'Partly cloudy', 'Cloudy'][code];
  return cap(cond(code).toLowerCase().replace("t'storms", 'thunderstorms likely'));
}
function precipWord(code) {
  if (code >= 95) return 'thunderstorms';
  if ([71, 73, 75, 77, 85, 86].includes(code)) return 'snow';
  if ([56, 57, 66, 67].includes(code)) return 'freezing rain';
  if (code >= 80) return 'showers';
  return 'rain';
}
function circularMean(degs) {
  let x = 0, y = 0;
  for (const d of degs) { x += Math.cos(d * Math.PI / 180); y += Math.sin(d * Math.PI / 180); }
  return (Math.atan2(y, x) * 180 / Math.PI + 360) % 360;
}

/** The next four moon phases after `ms`, from the mean synodic month (good to about a day). */
function moonPhases(ms) {
  const SYN = 29.530588853 * 864e5, REF = Date.UTC(2000, 0, 6, 18, 14);
  const names = ['New', 'First', 'Full', 'Last'];
  const out = [];
  let n = Math.floor((ms - REF) / SYN);
  for (let q = 0; out.length < 4; q++) {
    const t = REF + (n + q / 4) * SYN;
    if (t >= ms - 864e5) out.push({ name: names[q % 4], ms: t, q: q % 4 });
    if (q % 4 === 3) { n++; q = -1; }
  }
  return out;
}

// ---------------------------------------------------------------------------------------
// Screens (SVG). Laid out on a 640x480 stage, scaled to the frame height and centred; the
// background, header line and crawl bar run the full frame width.

const C = {
  bgTop: '#0c1660', bgBottom: '#3a1d76', title: '#ffe04a', text: '#ffffff', label: '#a9c0ff',
  gold: '#f2a422', goldLight: '#ffd94a', panelTop: '#22308f', panelBottom: '#4a2f9a', panelEdge: '#8fa6ff', bar: '#13238a',
};
const FONT = 'Liberation Sans, Arial, Helvetica, sans-serif';
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]);
function txt(x, y, s, { size = 24, fill = C.text, anchor = 'start', weight = 'bold', shadow = 2 } = {}) {
  const a = `x="__X__" y="${y}" font-family="${FONT}" font-size="${size}" font-weight="${weight}" text-anchor="${anchor}"`;
  const one = (dx, color) => `<text ${a.replace('__X__', x + dx).replace(`y="${y}"`, `y="${y + dx}"`)} fill="${color}">${esc(s)}</text>`;
  return (shadow ? one(shadow, '#000000') : '') + one(0, fill);
}

function icon(kind, cx, cy, s = 1) {
  const sun = (x, y, r) => {
    let rays = '';
    for (let i = 0; i < 8; i++) {
      const a = (i * Math.PI) / 4;
      rays += `<line x1="${x + Math.cos(a) * r * 1.35}" y1="${y + Math.sin(a) * r * 1.35}" x2="${x + Math.cos(a) * r * 1.8}" y2="${y + Math.sin(a) * r * 1.8}" stroke="${C.goldLight}" stroke-width="${r * 0.28}" stroke-linecap="round"/>`;
    }
    return `${rays}<circle cx="${x}" cy="${y}" r="${r}" fill="${C.goldLight}" stroke="${C.gold}" stroke-width="${r * 0.15}"/>`;
  };
  const moon = (x, y, r) => `<path d="M ${x + r * 0.3} ${y - r} A ${r} ${r} 0 1 0 ${x + r * 0.3} ${y + r} A ${r * 0.75} ${r * 0.75} 0 1 1 ${x + r * 0.3} ${y - r} Z" fill="#fff4b0" stroke="${C.gold}" stroke-width="2"/>`;
  const cloud = (x, y, w, fill = '#f4f6ff', edge = '#9aa6d6') => {
    const r = w / 4;
    return `<g fill="${fill}" stroke="${edge}" stroke-width="2"><ellipse cx="${x}" cy="${y + r * 0.4}" rx="${w / 2}" ry="${r * 0.9}"/>` +
      `<circle cx="${x - r * 0.8}" cy="${y}" r="${r}"/><circle cx="${x + r * 0.5}" cy="${y - r * 0.4}" r="${r * 1.25}"/></g>` +
      `<ellipse cx="${x}" cy="${y + r * 0.35}" rx="${w / 2 - 2}" ry="${r * 0.8}" fill="${fill}"/>`;
  };
  const drops = (x, y, color, n = 3) => Array.from({ length: n }, (_, i) =>
    `<line x1="${x - 18 + i * 18}" y1="${y}" x2="${x - 24 + i * 18}" y2="${y + 16}" stroke="${color}" stroke-width="4" stroke-linecap="round"/>`).join('');
  const flakes = (x, y) => Array.from({ length: 3 }, (_, i) => txt(x - 18 + i * 18, y + 16, '*', { size: 26, anchor: 'middle', shadow: 0 })).join('');
  let g;
  switch (kind) {
    case 'sun': g = sun(0, 0, 22); break;
    case 'moon': g = moon(0, 0, 24); break;
    case 'partly': g = sun(-12, -12, 17) + cloud(8, 10, 56); break;
    case 'partlyNight': g = moon(-12, -12, 18) + cloud(8, 10, 56); break;
    case 'cloud': g = cloud(-10, -6, 50, '#c9cfe6') + cloud(8, 8, 60); break;
    case 'fog': g = cloud(0, -8, 60, '#c9cfe6') + [12, 22, 32].map(y => `<line x1="-30" y1="${y}" x2="30" y2="${y}" stroke="#dfe3f5" stroke-width="5" stroke-linecap="round"/>`).join(''); break;
    case 'rain': g = cloud(0, -12, 64, '#c9cfe6', '#7f88b0') + drops(0, 14, '#5fb0ff'); break;
    case 'sleet': g = cloud(0, -12, 64, '#c9cfe6', '#7f88b0') + drops(0, 14, '#5fb0ff', 2) + txt(18, 30, '*', { size: 26, anchor: 'middle', shadow: 0 }); break;
    case 'snow': g = cloud(0, -12, 64, '#e6e9f7') + flakes(0, 14); break;
    case 'storm': g = cloud(0, -12, 64, '#9ea6c8', '#5d6590') + `<path d="M 4 4 L -10 24 L 0 24 L -6 42 L 14 16 L 3 16 L 10 4 Z" fill="${C.goldLight}" stroke="${C.gold}" stroke-width="2"/>`; break;
    default: g = cloud(0, 0, 60);
  }
  return `<g transform="translate(${cx} ${cy}) scale(${s})">${g}</g>`;
}

/** The parts of every screen. `title` is one or two lines. */
function frame(size, title, body, dateLine) {
  const { w, h } = size;
  const s = h / 480, ox = (w - 640 * s) / 2;
  const lines = title.split('\n');
  const head = lines.length > 1
    ? txt(132, 44, lines[0], { size: 30, fill: C.title }) + txt(132, 78, lines[1], { size: 30, fill: C.title })
    : txt(132, 62, lines[0], { size: 32, fill: C.title });
  const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="${w}" height="${h}" viewBox="0 0 ${w} ${h}">
<defs>
 <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.bgTop}"/><stop offset="1" stop-color="${C.bgBottom}"/></linearGradient>
 <linearGradient id="panel" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${C.panelTop}"/><stop offset="1" stop-color="${C.panelBottom}"/></linearGradient>
 <linearGradient id="gold" x1="0" y1="0" x2="1" y2="0"><stop offset="0" stop-color="${C.gold}"/><stop offset="0.5" stop-color="${C.goldLight}"/><stop offset="1" stop-color="${C.gold}"/></linearGradient>
 <linearGradient id="logo" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3a6cf0"/><stop offset="1" stop-color="#1b2f9a"/></linearGradient>
</defs>
<rect width="${w}" height="${h}" fill="url(#bg)"/>
<rect x="0" y="${92 * s}" width="${w}" height="${4 * s}" fill="url(#gold)"/>
<rect x="0" y="${418 * s}" width="${w}" height="${62 * s}" fill="${C.bar}"/>
<rect x="0" y="${418 * s}" width="${w}" height="${3 * s}" fill="url(#gold)"/>
<g transform="translate(${ox} 0) scale(${s})">
 <rect x="34" y="18" width="84" height="64" rx="8" fill="url(#logo)" stroke="${C.goldLight}" stroke-width="3"/>
 ${icon('partly', 70, 44, 0.62)}
 ${txt(104, 76, 'WX', { size: 15, fill: C.goldLight, anchor: 'middle', shadow: 1 })}
 ${head}
 ${dateLine ? txt(606, 78, dateLine, { size: 18, anchor: 'end' }) : ''}
 ${body}
</g>
</svg>`;
  return svg;
}
const panel = (x = 36, y = 108, w = 568, h = 298) =>
  `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="6" fill="url(#panel)" stroke="${C.panelEdge}" stroke-width="3"/>` +
  `<rect x="${x + 5}" y="${y + 5}" width="${w - 10}" height="${h - 10}" rx="4" fill="none" stroke="#000000" stroke-opacity="0.35" stroke-width="2"/>`;

function wrapWords(text, width) {
  const lines = [];
  let line = '';
  for (const word of text.split(/\s+/)) {
    if (line && (line + ' ' + word).length > width) { lines.push(line); line = word; } else line = line ? `${line} ${word}` : word;
  }
  if (line) lines.push(line);
  return lines;
}

function currentPage(d, place) {
  const c = d.current;
  const t = c.temperature_2m, wind = c.wind_speed_10m;
  const feelsLabel = !METRIC && t <= 50 && wind > 3 ? 'Wind Chill' : !METRIC && t >= 80 ? 'Heat Index' : 'Feels Like';
  const p = d.hourly.pressure_msl, i = d.hourly.time.indexOf(c.time.slice(0, 13) + ':00');
  const trend = i >= 3 && Number.isFinite(p[i - 3]) ? (c.pressure_msl - p[i - 3] > 0.5 ? ' ↑' : c.pressure_msl - p[i - 3] < -0.5 ? ' ↓' : '') : '';
  const rows = [
    ['Humidity:', `${r0(c.relative_humidity_2m)}%`],
    ['Dewpoint:', `${r0(c.dew_point_2m)}°`],
    [`${feelsLabel}:`, `${r0(c.apparent_temperature)}°`],
    ['Visibility:', `${visibility(c.visibility)} ${U.dist}.`],
    ['Pressure:', `${pressure(c.pressure_msl)}${trend}`],
    ['Wind:', wind < 1 ? 'Calm' : `${dir16(c.wind_direction_10m)} ${r0(wind)}`],
    ['Gusts:', c.wind_gusts_10m > wind + 5 ? `${r0(c.wind_gusts_10m)} ${U.speed}` : 'None'],
  ];
  const day = c.is_day === 1;
  let body = panel() + txt(176, 148, place.name, { size: 26, fill: C.title, anchor: 'middle' });
  body += txt(176, 232, `${r0(t)}°`, { size: 76, anchor: 'middle', shadow: 3 });
  body += icon(iconOf(c.weather_code, day), 176, 300, 1.1);
  body += txt(176, 382, cond(c.weather_code, day), { size: 28, anchor: 'middle' });
  body += `<line x1="320" y1="126" x2="320" y2="388" stroke="${C.panelEdge}" stroke-opacity="0.5" stroke-width="2"/>`;
  rows.forEach(([k, v], n) => {
    body += txt(340, 154 + n * 36, k, { size: 23, fill: C.label }) + txt(588, 154 + n * 36, v, { size: 23, anchor: 'end' });
  });
  return { title: 'Current\nConditions', body };
}

function forecastPages(d, ms) {
  const ps = periods(d, ms);
  const pages = [];
  for (let n = 0; n < ps.length; n += 2) {
    let body = panel(), y = 156;
    for (const p of ps.slice(n, n + 2)) {
      for (const line of wrapWords(`${p.name.toUpperCase()}...${p.text}`, 33)) { body += txt(58, y, line, { size: 29 }); y += 37; }
      y += 18;
    }
    pages.push({ title: 'Local\nForecast', body });
  }
  return pages;
}

function extendedPage(d, ms) {
  const { date } = localParts(ms, d.timezone);
  const i0 = d.daily.time.indexOf(date) + 1;
  let body = '';
  for (let n = 0; n < 3; n++) {
    const i = i0 + n, iso = d.daily.time[i];
    if (!iso) break;
    const x = 36 + n * 192, cx = x + 92;
    body += panel(x, 108, 184, 298);
    body += txt(cx, 146, DAYS[weekday(iso)].slice(0, 3).toUpperCase(), { size: 30, fill: C.title, anchor: 'middle' });
    body += icon(iconOf(d.daily.weather_code[i]), cx, 210, 1);
    const words = wrapWords(cond(d.daily.weather_code[i]), 12);
    words.slice(0, 2).forEach((l, k) => { body += txt(cx, 284 + k * 27, l, { size: 23, anchor: 'middle' }); });
    body += txt(cx - 40, 352, 'Lo', { size: 20, fill: C.label, anchor: 'middle' }) + txt(cx + 40, 352, 'Hi', { size: 20, fill: C.label, anchor: 'middle' });
    body += txt(cx - 40, 390, r0(d.daily.temperature_2m_min[i]), { size: 34, anchor: 'middle' }) + txt(cx + 40, 390, r0(d.daily.temperature_2m_max[i]), { size: 34, anchor: 'middle' });
  }
  return { title: 'Extended\nForecast', body };
}

function almanacPage(d, ms) {
  const { date } = localParts(ms, d.timezone);
  const i = Math.max(0, d.daily.time.indexOf(date));
  let body = panel();
  [i, i + 1].forEach((di, n) => {
    const iso = d.daily.time[di];
    if (!iso) return;
    body += txt(330 + n * 150, 150, DAYS[weekday(iso)], { size: 24, fill: C.title, anchor: 'middle' });
    body += txt(330 + n * 150, 186, hhmm(d.daily.sunrise[di]), { size: 24, anchor: 'middle' });
    body += txt(330 + n * 150, 220, hhmm(d.daily.sunset[di]), { size: 24, anchor: 'middle' });
  });
  body += txt(70, 186, 'Sunrise:', { size: 24, fill: C.label }) + txt(70, 220, 'Sunset:', { size: 24, fill: C.label });
  body += `<line x1="56" y1="240" x2="584" y2="240" stroke="${C.panelEdge}" stroke-opacity="0.5" stroke-width="2"/>`;
  body += txt(320, 272, 'Moon Data:', { size: 24, fill: C.title, anchor: 'middle' });
  moonPhases(ms).forEach((m, n) => {
    const cx = 110 + n * 140;
    body += txt(cx, 302, m.name, { size: 22, anchor: 'middle' });
    body += moonIcon(m.q, cx, 338);
    const dt = new Date(m.ms);
    const md = new Intl.DateTimeFormat('en-US', { timeZone: d.timezone, month: 'short', day: 'numeric' }).format(dt);
    body += txt(cx, 390, md, { size: 22, anchor: 'middle' });
  });
  return { title: 'Almanac', body };
}
function moonIcon(q, x, y, r = 20) {
  const lit = '#fff4b0', dark = '#1a1f4a';
  const disc = `<circle cx="${x}" cy="${y}" r="${r}" fill="${q === 2 ? lit : dark}" stroke="${C.gold}" stroke-width="2"/>`;
  if (q === 0 || q === 2) return disc;
  // quarter: light the right half (first quarter) or the left half (last quarter)
  const sweep = q === 1 ? 1 : 0;
  return disc + `<path d="M ${x} ${y - r} A ${r} ${r} 0 0 ${sweep} ${x} ${y + r} Z" fill="${lit}"/>`;
}

function noDataPage() {
  return { title: 'Local\nForecast', body: panel() + txt(320, 240, 'Weather information is', { size: 28, anchor: 'middle' }) + txt(320, 278, 'temporarily unavailable', { size: 28, anchor: 'middle' }) };
}

/** What the crawl along the bottom says. */
function crawl(d, place) {
  if (!d) return 'The local forecast will return shortly.';
  const c = d.current;
  return `Conditions at ${place.name}: ${r0(c.temperature_2m)}°${U.temp}  ${cond(c.weather_code, c.is_day === 1)}     ` +
    `Humidity: ${r0(c.relative_humidity_2m)}%     Dewpoint: ${r0(c.dew_point_2m)}°     ` +
    `Barometric Pressure: ${pressure(c.pressure_msl)} ${U.pressure}.     ` +
    `Wind: ${c.wind_speed_10m < 1 ? 'Calm' : `${dir16(c.wind_direction_10m)} ${r0(c.wind_speed_10m)} ${U.speed.toUpperCase()}`}     ` +
    `Visibility: ${visibility(c.visibility)} ${U.dist}.`;
}

// ---------------------------------------------------------------------------------------
// Rendering

const PAGE_SECONDS = { current: 12, forecast: 12, extended: 12, almanac: 10 };

/** Plans a weather segment from `entry.offsetExact` to its end (render.js calls this). */
export function weatherPlan(entry, ctx, size) {
  const it = entry.item;
  const ms = entry.startMs + entry.offsetExact * 1000;
  const d = wx?.data || null, place = wx?.place || { name: 'Your Area' };
  const tz = d?.timezone;
  const dateLine = new Intl.DateTimeFormat('en-US', { timeZone: tz, weekday: 'short', month: 'short', day: 'numeric' }).format(new Date(ms)).toUpperCase().replace(',', '');
  const screens = d
    ? [[currentPage(d, place), PAGE_SECONDS.current], ...forecastPages(d, ms).map(p => [p, PAGE_SECONDS.forecast]),
      [extendedPage(d, ms), PAGE_SECONDS.extended], [almanacPage(d, ms), PAGE_SECONDS.almanac]]
    : [[noDataPage(), 30]];
  const pages = screens.map(([p, seconds]) => ({ svg: frame(size, p.title, p.body, dateLine), seconds }));

  // Songs from the offset on: the one playing, seeked into, then the rest.
  const offset = entry.offsetExact, duration = Math.max(0.5, entry.remaining);
  const audio = [];
  for (const s of it.music || []) {
    const end = s.fromS + s.durationS;
    if (end <= offset || s.fromS >= offset + duration) continue;
    audio.push({ url: archiveUrl(s.id, s.file), seek: Math.max(0, offset - s.fromS), from: Math.max(0, s.fromS - offset) });
  }
  return { kind: 'weather', size, pages, offset, duration, audio, crawl: crawl(d, place), clockEpoch: ms / 1000 };
}

/** ffmpeg arguments for a weather plan: the page loop, the live clock and crawl, the music. */
export function weatherArgs(p) {
  const { w, h } = p.size || SIZES['16x9'];
  const s = h / 480, ox = (w - 640 * s) / 2;
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'spelunkr-wx-'));
  p.pages.forEach((pg, i) => fs.writeFileSync(path.join(dir, `p${i}.svg`), pg.svg));

  // The page loop, starting at the right page and the right moment within it.
  const loop = p.pages.reduce((t, pg) => t + pg.seconds, 0);
  let t = p.offset % loop, i = 0;
  while (t >= p.pages[i].seconds) { t -= p.pages[i].seconds; i++; }
  const list = [];
  let left = p.duration, first = true;
  while (left > 0) {
    const pg = p.pages[i];
    const len = Math.min(left, first ? pg.seconds - t : pg.seconds);
    list.push(`file 'p${i}.svg'`, `duration ${len.toFixed(3)}`);
    left -= len; first = false; i = (i + 1) % p.pages.length;
  }
  list.push(list[list.length - 2]); // the concat demuxer drops the last entry's duration otherwise
  const listFile = path.join(dir, 'pages.txt');
  fs.writeFileSync(listFile, list.join('\n') + '\n');

  const fontfile = font().replace(/[:\\']/g, m => '\\' + m);
  const crawlFile = path.join(dir, 'crawl.txt');
  fs.writeFileSync(crawlFile, `${p.crawl}          ${p.crawl}          `);
  const fs1 = Math.round(26 * s), fs2 = Math.round(22 * s);
  const shadow = `shadowcolor=black:shadowx=${Math.max(1, Math.round(2 * s))}:shadowy=${Math.max(1, Math.round(2 * s))}`;
  const clock = `drawtext=fontfile='${fontfile}':fontsize=${fs1}:fontcolor=white:${shadow}:` +
    `x=${(ox + 606 * s).toFixed(0)}-text_w:y=${(28 * s).toFixed(0)}:text='%{pts\\:localtime\\:${p.clockEpoch.toFixed(3)}\\:%l\\\\\\:%M\\\\\\:%S %p}'`;
  const ticker = `drawtext=fontfile='${fontfile}':textfile='${crawlFile}':expansion=none:fontsize=${fs2}:fontcolor=white:${shadow}:` +
    `y=${(436 * s).toFixed(0)}:x=w-mod(t*${Math.round(95 * s)}\\,w+text_w)`;
  const vchain = `[0:v]fps=${OUT.fps},scale=${w}:${h},setsar=1,${clock},${ticker},format=yuv420p[v]`;

  const inputs = ['-f', 'concat', '-safe', '0', '-i', listFile];
  let achain;
  if (p.audio.length) {
    const parts = [];
    p.audio.forEach((a, n) => {
      inputs.push('-ss', a.seek.toFixed(3), ...httpIn(a.url));
      const until = (p.audio[n + 1]?.from ?? p.duration) - a.from;
      parts.push(`[${n + 1}:a]aresample=${OUT.rate},atrim=0:${until.toFixed(3)},apad=whole_dur=${until.toFixed(3)},asetpts=PTS-STARTPTS[a${n}]`);
    });
    const fade = Math.min(1.5, p.duration / 4);
    achain = parts.join(';') + ';' + p.audio.map((_, n) => `[a${n}]`).join('') +
      `concat=n=${p.audio.length}:v=0:a=1,${loudnorm(LOUDNESS.card)},afade=t=out:st=${(p.duration - fade).toFixed(3)}:d=${fade.toFixed(3)}[a]`;
  } else {
    inputs.push('-f', 'lavfi', '-i', `anullsrc=r=${OUT.rate}:cl=stereo`);
    achain = `[1:a]atrim=0:${p.duration.toFixed(3)}[a]`;
  }
  return {
    args: ['-hide_banner', '-loglevel', 'error', '-nostdin', ...inputs, '-filter_complex', `${vchain};${achain}`,
      '-map', '[v]', '-map', '[a]', '-t', p.duration.toFixed(3), ...encodeArgs()],
    cleanup() { fs.rmSync(dir, { recursive: true, force: true }); },
  };
}
