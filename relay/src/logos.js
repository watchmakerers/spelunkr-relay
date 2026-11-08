// Channel logomarks: 32x32 pixel art whose letters name palette colours. Mirrors the site's
// pixel logo palette (network colours plus three derived from the channel colour) so the PS3
// guide draws the same logos.

const NETWORK = { k: '#0b0f26', n: '#2a3580', s: '#8899cc', p: '#e8eaf0', g: '#ffd740', o: '#d2500c' };
const SHADE = { hueToward: 255, hueStep: 14, satMul: 1.1, valueMul: 0.6 };
const LIGHT = { hueToward: 50, hueStep: 10, satMul: 0.6, valueLift: 0.6 };

function hexToHsv(hex) {
  const [r, g, b] = [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16) / 255);
  const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
  let h = 0;
  if (d) h = max === r ? ((g - b) / d + (g < b ? 6 : 0)) / 6 : max === g ? ((b - r) / d + 2) / 6 : ((r - g) / d + 4) / 6;
  return [h * 360, max ? (d / max) * 100 : 0, max * 100];
}

function hsvToHex(h, s, v) {
  s /= 100; v /= 100;
  const ch = n => {
    const k = (n + h / 60) % 6;
    return Math.round(255 * (v - v * s * Math.max(0, Math.min(k, 4 - k, 1)))).toString(16).padStart(2, '0');
  };
  return `#${ch(5)}${ch(3)}${ch(1)}`;
}

function nudgeHue(h, toward, step) {
  const d = ((toward - h + 540) % 360) - 180;
  return (h + Math.sign(d) * Math.min(step, Math.abs(d)) + 360) % 360;
}

export function logoPalette(color) {
  const c = /^#[0-9a-f]{6}$/i.test(color || '') ? color.toLowerCase() : '#666666';
  const [h, s, v] = hexToHsv(c);
  return {
    ...NETWORK,
    c,
    d: hsvToHex(nudgeHue(h, SHADE.hueToward, SHADE.hueStep), Math.min(100, s * SHADE.satMul), v * SHADE.valueMul),
    l: hsvToHex(nudgeHue(h, LIGHT.hueToward, LIGHT.hueStep), s * LIGHT.satMul, v + (100 - v) * LIGHT.valueLift),
  };
}

/** { rows: string[], palette: {letter: '#rrggbb'} } or null when the channel has no pixel logo. */
export function pixelLogo(channel) {
  const m = channel.logomark;
  if (!m || m.type !== 'pixel' || !Array.isArray(m.rows) || !m.rows.length) return null;
  if (!m.rows.every(r => typeof r === 'string' && r.length === m.rows.length)) return null;
  const pal = logoPalette(channel.color);
  const used = new Set(m.rows.join('').replace(/\./g, ''));
  const palette = {};
  for (const k of used) palette[k] = pal[k] || pal.p;
  return { rows: m.rows, palette };
}
