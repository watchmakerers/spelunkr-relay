// Pulls Spelunkr's own scheduling engine out of the site's production bundle.
//
// spelunkr.tv has no "what's on" API: every browser computes the schedule itself from
// lineup.json + per-channel manifests, using seeded shuffles so all viewers agree. To stay in
// sync with the real site we run *their* code rather than a re-implementation. The bundle is
// minified, so we locate the engine by the shape of its code (not by its short names, which
// change on every build) and evaluate just that slice in a sandbox.

import vm from 'node:vm';

const SITE = 'https://spelunkr.tv/';

// Each anchor captures the minified name of a function we need to call.
const EXPORTS = {
  // (channelId, versions[]) -> playlist
  createPlaylist: /function ([\w$]+)\(e,t=\[\]\)\{const n=\{channelId:e,versions:\[\]\}/,
  // (playlist, ms) -> { item, startMs, endMs, offsetExact, remaining, ... }
  entryAt: /function ([\w$]+)\(e,t\)\{const n=[\w$]+\(e,t\);if\(!n\)throw new Error\(`\$\{e\.channelId\}: no published version`\)/,
  // generator: (playlist, ms) -> successive entries
  walk: /function\*([\w$]+)\(e,t\)\{let n=t;for\(;;\)\{const r=[\w$]+\(e,n\);yield r,n=r\.endMs\}\}/,
  // (manifest, platform, versionPointer) -> version
  buildVersion: /function ([\w$]+)\(e,t,n\)\{const r=e\.bumps\|\|\[\],s=\(t==null\?void 0:t\.bumps\)\|\|\[\]/,
};

// The engine runs from the block of timing constants that precedes the slide-length helper
// through the archive.org URL helper.
const START = /const [\w$]+=\d+,[\w$]+=\d+,[\w$]+=\d+,[\w$]+=\d+,[\w$]+=[\w$.]+,[\w$]+=\d+,[\w$]+=\d+e\d,[^;]*;function [\w$]+\(e\)\{return Math\.max\(/;
const END = /function [\w$]+\(e,t\)\{return`https:\/\/archive\.org\/download\/\$\{e\}\/\$\{encodeURIComponent\(t\)\}`\}/;

function sandbox() {
  return vm.createContext({
    console, Math, Date, Map, Set, WeakMap, Number, String, Array, Object, JSON, parseInt,
    parseFloat, Error, TypeError, RangeError, TextEncoder, Uint8Array, Uint32Array, DataView,
    Symbol, Infinity, NaN, isFinite, isNaN, encodeURIComponent, decodeURIComponent,
  });
}

/** Returns the engine's source (sliced from `bundle`) with a footer that exposes the API. */
export function sliceEngine(bundle) {
  const start = bundle.search(START);
  const endMatch = END.exec(bundle);
  if (start < 0 || !endMatch) throw new Error('engine boundaries not found in bundle');
  const end = endMatch.index + endMatch[0].length;
  let src = bundle.slice(start, end);

  const names = {};
  for (const [api, re] of Object.entries(EXPORTS)) {
    const m = re.exec(bundle);
    if (!m) throw new Error(`engine function "${api}" not found in bundle`);
    names[api] = m[1];
    // buildVersion lives outside the main slice; append its definition.
    if (m.index < start || m.index >= end) src += '\n' + functionAt(bundle, m.index);
  }
  const footer = Object.entries(names).map(([k, v]) => `${k}:${v}`).join(',');
  return `${src}\n;globalThis.__spelunkrEngine={${footer}};`;
}

// Returns the complete `function name(...){...}` that starts at `i`, by brace matching
// (the functions we lift contain no braces inside strings/regexes that would confuse this).
function functionAt(src, i) {
  let depth = 0;
  for (let j = src.indexOf('{', i); j < src.length; j++) {
    if (src[j] === '{') depth++;
    else if (src[j] === '}' && --depth === 0) return src.slice(i, j + 1);
  }
  throw new Error('unbalanced function');
}

export function loadEngine(source) {
  const ctx = sandbox();
  vm.runInContext(source, ctx, { filename: 'spelunkr-engine.js' });
  const e = ctx.__spelunkrEngine;
  for (const k of Object.keys(EXPORTS)) if (typeof e[k] !== 'function') throw new Error(`engine missing ${k}`);
  return e;
}

/** Fetches the live site's bundle; returns { build, source }. */
export async function fetchEngineSource() {
  const html = await (await fetch(SITE)).text();
  const m = /<script[^>]+src="(\/assets\/index-[\w-]+\.js)"/.exec(html);
  if (!m) throw new Error('could not find the app bundle on spelunkr.tv');
  const bundle = await (await fetch(new URL(m[1], SITE))).text();
  const engineVersion = /,([\w$]+)=(\d+),[\w$]+=1,[\w$]+=2,[\w$]+=5,/.exec(bundle);
  return { build: m[1], source: sliceEngine(bundle), maxEngine: engineVersion ? Number(engineVersion[2]) : null };
}
