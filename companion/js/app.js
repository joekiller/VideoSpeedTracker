/**
 * App shell: pairing, connection state, hash routing, and app-wide reactions to live events.
 *
 * Boot flow:
 *   1. A QR code from VST Studio opens `http://<lan-ip>:8787/#token=…`; the token is saved on the phone
 *      and the fragment is replaced so it never sits in history or a shared screenshot.
 *   2. `vst.probe()` checks the saved target (same origin by default). Reachable → live events start and
 *      the requested screen renders; otherwise the Connect screen explains what to do, and offers the demo.
 *   3. Each route renders into <main> and may return a cleanup function, run before the next route.
 */
import { Vst } from './api.js';
import { $, $$, html, sign, band, toast, BAND_LABEL } from './ui.js';
import { prefs } from './prefs.js';
import * as live from './views/live.js';
import * as passes from './views/passes.js';
import * as stats from './views/stats.js';
import * as setup from './views/setup.js';
import * as system from './views/system.js';
import * as connect from './views/connect.js';
import * as map from './views/map.js';

const vst = new Vst();
const view = $('#view');
const ctx = { vst, view, prefs, go: (h) => { location.hash = h; } };
let cleanup = null;
let here = location.hash;   // the hash on screen now, for the leave check below
let unseen = 0;

const ROUTES = [
  [/^#\/live$/, live, 'live'],
  [/^#\/passes(?:\/(\d+))?$/, passes, 'passes'],
  [/^#\/stats$/, stats, 'stats'],
  [/^#\/setup(?:\/(\d))?$/, setup, 'setup'],
  [/^#\/system$/, system, 'system'],
  [/^#\/map$/, map, 'setup'],
  [/^#\/connect$/, connect, null],
];

/** Renders the screen for the current hash, after the previous screen cleans up. */
async function route() {
  here = location.hash;
  const hash = location.hash || '#/live';
  const [re, mod, tab] = ROUTES.find(([r]) => r.test(hash)) || ROUTES[0];
  const params = hash.match(re)?.slice(1) ?? [];
  if (!ctx.connected && mod !== connect) { ctx.go('#/connect'); return; }
  cleanup?.();
  cleanup = null;
  ctx.leaving = null;
  $$('#tabs a').forEach((a) => (a.dataset.tab === tab ? a.setAttribute('aria-current', 'page') : a.removeAttribute('aria-current')));
  $('#tabs').hidden = !ctx.connected;
  if (tab === 'passes') setUnseen(0);
  window.scrollTo({ top: 0 });
  try {
    cleanup = (await mod.render(ctx, ...params)) || null;
  } catch (e) {
    view.innerHTML = html`<div class="empty-state"><h2>Something went wrong</h2><p>${e.message}</p>
      <button class="btn" onclick="location.reload()">Reload</button></div>`.__html;
  }
}

function setUnseen(n) {
  unseen = n;
  const b = $('#new-badge');
  b.hidden = !n;
  b.textContent = n > 99 ? '99+' : n;
}

/** Header: site name and the connection pill. */
function paintHeader() {
  const s = vst.lastStatus;
  const c = $('#conn');
  let state = 'offline', label = 'Offline';
  if (!ctx.connected) label = 'Not connected';
  else if (vst.mode === 'demo') { state = 'demo'; label = 'Demo'; }
  else if (ctx.online === false) label = 'Reconnecting…';
  else if (s && s.running && !s.paused) { state = 'live'; label = 'Live'; }
  else if (s) { state = 'paused'; label = s.running ? 'Paused' : 'Stopped'; }
  c.dataset.state = state;
  c.lastElementChild.textContent = label;
  $('#site-name').textContent = s?.siteName || 'VST';
  // The desktop's unattended-health warnings take the subtitle's place: they need someone at the window.
  const warning = s?.health?.cameraWarning ? '⚠ Camera moved? Check the aim' : s?.health?.lowDisk ? '⚠ Disk nearly full' : '';
  $('#site-sub').textContent = warning || (s ? `Limit ${s.speedLimit} mph${s.legalDirection !== 'both' ? ' · one-way' : ''}` : 'Companion');
  $('#site-sub').classList.toggle('warn', !!warning);
  document.title = s?.siteName ? `${s.siteName} · VST` : 'VST Companion';
}

/** Connects (or reconnects) to the saved target and routes accordingly. */
export async function start() {
  vst.disconnectLive();
  const result = await vst.probe();
  ctx.connected = result === 'ok';
  ctx.probeResult = result;
  if (ctx.connected) {
    await Promise.all([vst.status(), vst.config()]).catch(() => {});
    vst.connectLive();
    if (location.hash === '#/connect' || !location.hash) ctx.go('#/live');
  } else {
    ctx.go('#/connect');
  }
  paintHeader();
  route();
}
ctx.start = start;

// Live events that matter on every screen.
vst.addEventListener('status', paintHeader);
vst.addEventListener('online', () => { ctx.online = true; paintHeader(); });
vst.addEventListener('offline', () => { ctx.online = false; paintHeader(); });
// The desktop made a new pairing code: start() probes again and lands on Connect, which asks for a fresh scan.
vst.addEventListener('signed-out', () => start());
vst.addEventListener('vehicle', (e) => {
  const v = e.detail;
  if (!location.hash.startsWith('#/passes')) setUnseen(unseen + 1);
  // Toast only notable passes, and not on Live (it shows every pass on the video already).
  const b = band(v);
  if (b !== 'ok' && !location.hash.startsWith('#/live')) {
    toast(html`${sign(v.speed, b)}<div class="grow"><b>${BAND_LABEL[b]}</b><div class="muted small">${v.compass}-bound · just now</div></div>`,
      { href: `#/passes/${v.id}` });
  }
});

$('#conn').addEventListener('click', () => ctx.go(ctx.connected ? '#/system' : '#/connect'));

// A screen with unsaved changes sets ctx.leaving to an async check (true = go).  Any link off it, the tabs, Back or a
// typed hash, comes back here first: the old hash is put back while the check asks, then the new one is followed.
window.addEventListener('hashchange', async () => {
  const to = location.hash;
  if (ctx.leaving && to !== here) {
    history.replaceState(null, '', here);
    const go = await ctx.leaving();
    if (!go) return;
    ctx.leaving = null;
    history.pushState(null, '', to);
  }
  here = to;
  route();
});
window.addEventListener('beforeunload', (e) => { if (ctx.leaving) { e.preventDefault(); e.returnValue = ''; } });

// Pairing link from VST Studio's QR code: #token=… (optionally &host=…).  Studio's "Find it on the map" opens the
// same link on the computer with &to=map, which lands on the map instead of Live.
{
  const m = location.hash.match(/^#(?:\/connect\?)?token=([^&]+)(?:&host=([^&]+))?(?:&to=(\w+))?/);
  if (m) {
    vst.useLive(m[2] ? decodeURIComponent(m[2]) : '', decodeURIComponent(m[1]));
    history.replaceState(null, '', `${location.pathname}#/${m[3] === 'map' ? 'map' : 'live'}`);
  }
}

// Offline app shell; service workers only run on https or localhost, so plain-LAN use just skips it.
if ('serviceWorker' in navigator && window.isSecureContext) navigator.serviceWorker.register('sw.js').catch(() => {});

start();
