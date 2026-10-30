/**
 * Client for the VST companion API (docs/companion-api.md).
 *
 * `Vst` hides where the data comes from:
 *   - mode "live": HTTP to a VST Studio on the LAN (normally the same origin that served this app),
 *     with the pairing token as a Bearer header, or as ?token= on media URLs and the SSE stream;
 *   - mode "demo": the in-browser simulation in demo.js, with media mapped to bundled assets.
 *
 * Views only ever call these methods and `on()` for live events, so they never branch on the mode.
 */
import { createDemoVst } from './demo.js';

const STORE = 'vst.connection';

export class ApiError extends Error {
  constructor(status, message) { super(message); this.status = status; }
}

export class Vst extends EventTarget {
  constructor() {
    super();
    const saved = load();
    this.base = saved.base ?? '';        // '' = same origin as the app
    this.token = saved.token ?? '';
    this.mode = saved.mode ?? 'live';
    this.demo = null;
    this.stopLive = null;
    this.lastStatus = null;
    this.lastConfig = null;
  }

  // ---- connection -------------------------------------------------------------------------------

  /** Persists base/token/mode so the phone reconnects on the next launch. */
  save() { localStorage.setItem(STORE, JSON.stringify({ base: this.base, token: this.token, mode: this.mode })); }

  useDemo() {
    this.mode = 'demo';
    this.demo ??= createDemoVst({ tracks: false });
    this.save();
  }

  useLive(base, token) {
    this.mode = 'live';
    this.base = normalizeBase(base);
    if (token != null) this.token = token;
    this.save();
  }

  forget() {
    this.disconnectLive();
    this.token = '';
    this.mode = 'live';
    this.save();
  }

  /**
   * Checks the current target. Resolves to one of:
   *   'ok'       - reachable and the token works;
   *   'needs-token' - VST answered /api/hello but rejected the token;
   *   'unreachable' - nothing that looks like VST at this address.
   */
  async probe() {
    if (this.mode === 'demo') { this.useDemo(); return 'ok'; }
    try {
      const hello = await fetch(this.url('/api/hello'), { cache: 'no-store' }).then((r) => r.json());
      if (hello?.app !== 'VST') return 'unreachable';
    } catch { return 'unreachable'; }
    try { await this.status(); return 'ok'; } catch (e) { return e.status === 401 ? 'needs-token' : 'unreachable'; }
  }

  // ---- requests ---------------------------------------------------------------------------------

  url(path, withToken = false) {
    const u = `${this.base}${path}`;
    return withToken && this.token ? `${u}${u.includes('?') ? '&' : '?'}token=${encodeURIComponent(this.token)}` : u;
  }

  async request(method, path, body) {
    if (this.mode === 'demo') {
      const [p, q] = path.split('?');
      const r = this.demo.handle(method, p, new URLSearchParams(q || ''), body);
      if (r.status >= 400) throw new ApiError(r.status, r.json?.error || 'Request failed');
      return structuredClone(r.json ?? r.text);
    }
    const res = await fetch(this.url(path), {
      method,
      cache: 'no-store',
      headers: { ...(this.token && { Authorization: `Bearer ${this.token}` }), ...(body && { 'Content-Type': 'application/json' }) },
      body: body ? JSON.stringify(body) : undefined,
    });
    const isJson = res.headers.get('content-type')?.includes('json');
    const data = isJson ? await res.json() : await res.text();
    if (!res.ok) throw new ApiError(res.status, data?.error || `${res.status} ${res.statusText}`);
    return data;
  }

  status() { return this.request('GET', '/api/status').then((s) => (this.lastStatus = s)); }
  config() { return this.request('GET', '/api/config').then((c) => (this.lastConfig = c)); }
  async putConfig(partial) {
    const r = await this.request('PUT', '/api/config', partial);
    this.lastConfig = r.config;
    return r;
  }
  vehicles(params = {}) {
    const q = new URLSearchParams(Object.entries(params).filter(([, v]) => v != null && v !== ''));
    return this.request('GET', `/api/vehicles${q.size ? `?${q}` : ''}`);
  }
  vehicle(id) { return this.request('GET', `/api/vehicles/${id}`); }
  run(action) { return this.request('POST', '/api/run', { action }); }
  calibrationMark(dir, edge) { return this.request('POST', '/api/calibration/mark', { dir, edge }); }
  calibrationApply(body) { return this.request('POST', '/api/calibration/apply', body); }
  calibrationCheck(body) { return this.request('POST', '/api/calibration/check', body); }

  // ---- media ------------------------------------------------------------------------------------

  /** URL usable as <img>/<video> src for an API media path (adds the token, or maps to a demo asset). */
  media(path) {
    if (!path) return null;
    if (this.mode === 'demo') return this.demo.handle('GET', path.split('?')[0]).file ?? null;
    return this.url(path, true);
  }
  frameUrl() { return `${this.media('/api/frame.jpg?overlay=0')}${this.mode === 'demo' ? '' : `&t=${Date.now()}`}`; }
  /** Live view source: the MJPEG stream, or in demo mode a looping sample video. */
  liveSource() {
    return this.mode === 'demo' ? { kind: 'video', src: 'demo/clip.mp4' } : { kind: 'mjpeg', src: this.url('/api/stream.mjpg?overlay=0', true) };
  }
  csvUrl() { return this.mode === 'demo' ? null : this.url('/api/stats.csv', true); }
  /** The printable street report for local dates `from`..`to` ("YYYY-MM-DD", either may be empty = all data). */
  reportUrl({ from, to } = {}) {
    if (this.mode === 'demo') return 'demo/report.html';
    const q = new URLSearchParams(Object.entries({ from, to }).filter(([, v]) => v));
    return this.url(`/api/report.html${q.size ? `?${q}` : ''}`, true);
  }

  // ---- live events ------------------------------------------------------------------------------

  /** Subscribes to live events ('status', 'vehicle', 'tracks', 'config'), re-dispatched on this object. */
  connectLive() {
    this.disconnectLive();
    const fire = (type, data) => {
      if (type === 'status') this.lastStatus = data;
      if (type === 'config') this.lastConfig = data;
      this.dispatchEvent(new CustomEvent(type, { detail: data }));
    };
    if (this.mode === 'demo') { this.stopLive = this.demo.subscribe(fire); return; }
    let es = null, poll = null, lastId = null;   // null until the newest existing pass is known

    // SSE when VST offers it; on the first error fall back to polling, which every VST supports.
    const startPolling = () => {
      es?.close(); es = null;
      if (poll) return;
      const tick = async () => {
        try {
          fire('status', await this.status());
          // The first poll only learns the newest id; later polls announce what arrived since.
          const fresh = await this.vehicles(lastId == null ? { limit: 1 } : { since: lastId, limit: 50 });
          if (lastId != null) [...fresh].reverse().forEach((v) => fire('vehicle', v));
          lastId = Math.max(lastId ?? 0, ...fresh.map((v) => v.id));
          this.dispatchEvent(new CustomEvent('online'));
        } catch (e) {
          // 401: the pairing code was reset on the desktop, so retrying with this token can never work.
          if (e.status === 401) { this.disconnectLive(); this.dispatchEvent(new CustomEvent('signed-out')); }
          else this.dispatchEvent(new CustomEvent('offline', { detail: e }));
        }
      };
      tick();
      poll = setInterval(tick, 2500);
    };
    try {
      es = new EventSource(this.url('/api/live', true));
      let opened = false;
      es.onopen = () => { opened = true; this.dispatchEvent(new CustomEvent('online')); };
      es.onerror = async () => {
        if (!opened) { startPolling(); return; }
        this.dispatchEvent(new CustomEvent('offline'));
        // CONNECTING: a dropped connection that EventSource retries by itself.  CLOSED: VST answered the retry
        // with an error, which EventSource never retries.  That's a 401 after a new pairing code (which also ends
        // open streams), or a 503 when every stream slot is taken: find out which.
        if (es?.readyState !== EventSource.CLOSED) return;
        const result = await this.probe();
        if (result === 'needs-token') { this.disconnectLive(); this.dispatchEvent(new CustomEvent('signed-out')); }
        else if (result === 'ok') startPolling();
        else if (this.stopLive === stop) setTimeout(() => { if (this.stopLive === stop) this.connectLive(); }, 5000);   // VST is gone: try again
      };
      for (const t of ['status', 'vehicle', 'tracks', 'config']) es.addEventListener(t, (e) => fire(t, JSON.parse(e.data)));
    } catch { startPolling(); }
    const stop = () => { es?.close(); clearInterval(poll); };
    this.stopLive = stop;
  }

  disconnectLive() { this.stopLive?.(); this.stopLive = null; }
}

function load() {
  try { return JSON.parse(localStorage.getItem(STORE)) || {}; } catch { return {}; }
}

/** Accepts "192.168.1.20", "192.168.1.20:8787" or a full URL; '' stays same-origin. */
export function normalizeBase(input) {
  let s = (input || '').trim().replace(/\/+$/, '');
  if (!s) return '';
  if (!/^https?:\/\//i.test(s)) s = `http://${s}`;
  const u = new URL(s);
  if (!u.port && u.protocol === 'http:' && !/[:]\d+$/.test(s)) u.port = '8787';
  return u.origin === location.origin ? '' : u.origin;
}
