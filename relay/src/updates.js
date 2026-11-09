// Keeps a copy of the newest client release (checked every 30 minutes) so the apps can update themselves through the
// relay (consoles can't reach a private GitHub repo, or GitHub's TLS at all).
//
//   GET /update/latest        "VERSION 1.2\nFILE <name> <bytes>\n..." ("VERSION none" if unknown)
//   GET /update/file/<name>   one of those files
//
// The repo comes from SPELUNKR_REPO or the relay's own git remote; access uses GITHUB_TOKEN
// or the GitHub CLI's login (`gh auth token`), since the repo may be private.

import fs from 'node:fs';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const CACHE = path.join(HERE, '../cache/releases');
const CHECK_MS = 30 * 60e3;
// The raw files each client installs (CI attaches these to every release).
export const CLIENT_FILES = ['spelunkr-ps3-EBOOT.BIN', 'spelunkr-vita-eboot.bin', 'spelunkr-windows-x64.exe',
  'spelunkr-macos-bin', 'spelunkr-linux-x86_64-bin'];

function repo() {
  if (process.env.SPELUNKR_REPO) return process.env.SPELUNKR_REPO;
  try {
    const url = execFileSync('git', ['-C', HERE, 'remote', 'get-url', 'origin'], { encoding: 'utf8' }).trim();
    const m = /github\.com[:/]([^/]+\/[^/.]+)/.exec(url);
    return m ? m[1] : null;
  } catch { return null; }
}

function token() {
  if (process.env.GITHUB_TOKEN) return process.env.GITHUB_TOKEN;
  try { return execFileSync('gh', ['auth', 'token'], { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }).trim(); } catch { return null; }
}

export class Updates {
  constructor({ log = console } = {}) {
    this.log = log;
    this.latest = null; // { version, dir, files: [{ name, size }] }
  }

  start() {
    this.check().catch(e => this.log.warn(`[update] ${e.message}`));
    this.timer = setInterval(() => this.check().catch(e => this.log.warn(`[update] ${e.message}`)), CHECK_MS);
    this.timer.unref();
  }

  async check() {
    const r = repo(), t = token();
    if (!r) throw new Error('no GitHub repo known (set SPELUNKR_REPO=owner/name)');
    const headers = { 'user-agent': 'spelunkr-relay', accept: 'application/vnd.github+json', ...(t ? { authorization: `Bearer ${t}` } : {}) };
    const res = await fetch(`https://api.github.com/repos/${r}/releases/latest`, { headers });
    if (!res.ok) throw new Error(`${r}: latest release: HTTP ${res.status}${t ? '' : ' (no GitHub login: set GITHUB_TOKEN or run gh auth login)'}`);
    const rel = await res.json();
    const version = String(rel.tag_name || '').replace(/^v/, '');
    const dir = path.join(CACHE, version);
    fs.mkdirSync(dir, { recursive: true });
    const files = [];
    for (const a of rel.assets || []) {
      if (!CLIENT_FILES.includes(a.name)) continue;
      const file = path.join(dir, a.name);
      if (!fs.existsSync(file) || fs.statSync(file).size !== a.size) {
        const dl = await fetch(a.url, { headers: { ...headers, accept: 'application/octet-stream' } });
        if (!dl.ok) { this.log.warn(`[update] ${a.name}: HTTP ${dl.status}`); continue; }
        fs.writeFileSync(file + '.part', Buffer.from(await dl.arrayBuffer()));
        fs.renameSync(file + '.part', file);
      }
      files.push({ name: a.name, size: a.size });
    }
    if (!this.latest || this.latest.version !== version) this.log.info(`[update] clients ${version} ready (${files.length} files) from ${r}`);
    this.latest = { version, dir, files };
  }

  /** Handles /update/*; returns true if it answered. */
  handle(url, res) {
    if (url.pathname === '/update/latest') {
      res.writeHead(200, { 'content-type': 'text/plain', 'cache-control': 'no-store' });
      const l = this.latest;
      res.end(l ? [`VERSION ${l.version}`, ...l.files.map(f => `FILE ${f.name} ${f.size}`), ''].join('\n') : 'VERSION none\n');
      return true;
    }
    const m = /^\/update\/file\/([\w.-]+)$/.exec(url.pathname);
    if (m) {
      const f = this.latest?.files.find(x => x.name === m[1]);
      if (!f) { res.writeHead(404); res.end(); return true; }
      res.writeHead(200, { 'content-type': 'application/octet-stream', 'content-length': f.size });
      fs.createReadStream(path.join(this.latest.dir, f.name)).pipe(res);
      return true;
    }
    return false;
  }
}
