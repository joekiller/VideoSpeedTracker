/**
 * Pick your spot's map (#/map): OpenStreetMap, with two pins.
 *
 *   1. Find the house: type an address (OpenStreetMap's Nominatim search, sent only when the user presses Search) or
 *      use this device's location (only offered where the browser allows it: https or localhost).
 *   2. "Your window": tap where the camera is.  3. "The lane": tap the middle of the moving lane in front of it.
 *      Either pin can be dragged afterwards; the line between them shows the distance in feet.
 *   4. When the lane pin lands, OpenStreetMap's Overpass API is asked which street is under it: its name, its drawn
 *      direction at the nearest segment, one-way or not and the posted limit.  plan.js fromMap turns that into the
 *      planner's numbers (the same arithmetic as src/Gui/Plan.cpp, which VST Studio uses when the pins arrive).
 *   5. "Send to Pick your spot" PUTs {map: {...}} to /api/plan.  Like every planner change it is a preview (a draft)
 *      until it is saved on Pick your spot, here or in VST Studio.
 *
 * VST Studio's "Find it on the map" opens this page in the computer's browser (http://127.0.0.1:<port>/#token=…&to=map),
 * so it works the same on the computer and the phone.  No map library: a small slippy map is enough for two pins,
 * and keeps the companion free of a build step.  Tiles are OpenStreetMap's standard tiles (zoom 0-19), stretched past
 * 19 so two pins 20 ft apart can still be placed apart; the attribution is shown as the tile policy asks.
 */
import { html, $, $$, icon, toast, confirmSheet } from '../ui.js';
import * as plan from '../plan.js';

const TILE = 256, TILE_MAX_Z = 19, MAX_Z = 21, MIN_Z = 2;
const TILES = 'https://tile.openstreetmap.org';
const SEARCH = 'https://nominatim.openstreetmap.org/search';
const OVERPASS = 'https://overpass-api.de/api/interpreter';
// Ways that carry cars; footpaths and the like are skipped when finding the street under the lane pin.
const NOT_ROADS = /^(footway|path|cycleway|steps|pedestrian|track|bridleway|corridor|elevator|platform|proposed|construction)$/;

// Web Mercator: degrees <-> world pixels at zoom z.
const worldSize = (z) => TILE * 2 ** z;
const lonToX = (lon, z) => (lon + 180) / 360 * worldSize(z);
const latToY = (lat, z) => { const s = Math.sin(lat * Math.PI / 180); return (0.5 - Math.log((1 + s) / (1 - s)) / (4 * Math.PI)) * worldSize(z); };
const xToLon = (x, z) => x / worldSize(z) * 360 - 180;
const yToLat = (y, z) => { const n = Math.PI - 2 * Math.PI * y / worldSize(z); return 180 / Math.PI * Math.atan(Math.sinh(n)); };

/** "25 mph" -> 25; "40" (km/h, OpenStreetMap's default unit) -> 25; anything else -> 0. */
export function maxspeedMph(tag) {
  const m = String(tag || '').trim().match(/^(\d+(?:\.\d+)?)\s*(mph|km\/h|kmh)?$/i);
  if (!m) return 0;
  const v = Number(m[1]);
  return Math.round(/mph/i.test(m[2] || '') ? v : v * 0.621371);
}

/** OpenStreetMap's oneway tag as 1 (along the drawn direction), -1 (against it) or 0. */
export function onewayOf(tags = {}) {
  const o = String(tags.oneway || '').toLowerCase();
  if (o === '-1' || o === 'reverse') return -1;
  if (o === 'yes' || o === 'true' || o === '1') return 1;
  if (!o && (tags.junction === 'roundabout' || tags.highway === 'motorway')) return 1;
  return 0;
}

/**
 * The street nearest a point among Overpass ways ({tags, geometry: [{lat, lon}]}), within `maxM` metres: its name,
 * the bearing of the nearest segment in the way's drawn direction, one-way and posted limit.  null when none is near.
 */
export function nearestStreet(ways, lat, lon, maxM = 30) {
  const kx = 111320 * Math.cos(lat * Math.PI / 180), ky = 110540;
  let best = null;
  for (const w of ways || []) {
    if (!w.tags?.highway || NOT_ROADS.test(w.tags.highway) || !Array.isArray(w.geometry)) continue;
    for (let i = 1; i < w.geometry.length; i++) {
      const a = w.geometry[i - 1], b = w.geometry[i];
      const ax = (a.lon - lon) * kx, ay = (a.lat - lat) * ky, bx = (b.lon - lon) * kx, by = (b.lat - lat) * ky;
      const dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
      const t = len2 ? Math.max(0, Math.min(1, -(ax * dx + ay * dy) / len2)) : 0;
      const d = Math.hypot(ax + t * dx, ay + t * dy);
      if (d <= maxM && (!best || d < best.d)) best = { d, way: w, bearing: plan.bearingDeg(a.lat, a.lon, b.lat, b.lon) };
    }
  }
  if (!best) return null;
  const t = best.way.tags;
  return { street: t.name || t.ref || '', wayBearing: best.bearing, oneway: onewayOf(t), maxspeedMph: maxspeedMph(t.maxspeed), metres: best.d };
}

export async function render(ctx) {
  const { vst, view } = ctx;
  const p = await vst.plan().catch(() => null);
  const pins = { cam: null, lane: null };   // [lat, lon]
  if (p?.map) { pins.cam = [p.map.camLat, p.map.camLon]; pins.lane = [p.map.laneLat, p.map.laneLon]; }
  let way = null, wayState = pins.lane ? 'idle' : 'none';   // none | looking | found | missing | failed
  let which = pins.cam ? 'lane' : 'cam';
  let sent = true;   // nothing to lose yet
  // The view: centre in world pixels at zoom z.  Start on the pins, else on the continental US until a search.
  let z = pins.cam ? 20 : 4;
  let cx = lonToX(pins.cam ? (pins.cam[1] + (pins.lane?.[1] ?? pins.cam[1])) / 2 : -96, z);
  let cy = latToY(pins.cam ? (pins.cam[0] + (pins.lane?.[0] ?? pins.cam[0])) / 2 : 38.5, z);

  const canLocate = window.isSecureContext && 'geolocation' in navigator;
  view.innerHTML = html`
    <div class="step-kicker">Step 2 of 5 · Pick your spot · Map</div>
    <h1>Find your window on the map</h1>
    <p class="lede">Drop one pin on the window the camera looks out of and one in the middle of the lane in front of it. The map works out the distance and reads the street's name, one-way and limit.</p>
    <form class="row" id="search" style="gap:8px" role="search">
      <input class="input grow" id="q" type="search" placeholder="Your address, e.g. 329 7½ St SW, Charlottesville" aria-label="Address" autocomplete="street-address">
      <button class="btn" type="submit">Search</button>
    </form>
    <div class="list" id="results" hidden></div>
    ${canLocate ? html`<button class="btn ghost small" id="locate" style="margin-top:8px">${icon('compass')}Use where I am now</button>` : ''}
    <div class="seg" id="which" style="margin-top:12px" role="group" aria-label="Pin to place">
      <button data-v="cam"><i class="dot cam"></i>1. Your window</button><button data-v="lane"><i class="dot lane"></i>2. The lane</button>
    </div>
    <div class="map" id="map" tabindex="0" aria-label="Map. Drag to move, pinch or use + and − to zoom, tap to place the selected pin.">
      <div class="tiles" id="tiles"></div>
      <svg id="pins" aria-hidden="true"></svg>
      <div class="map-zoom"><button type="button" id="zin" aria-label="Zoom in">+</button><button type="button" id="zout" aria-label="Zoom out">−</button></div>
      <div class="map-attr">© <a href="https://www.openstreetmap.org/copyright" target="_blank" rel="noopener">OpenStreetMap</a> contributors</div>
    </div>
    <p class="small muted" id="tip" style="margin-top:8px"></p>
    <div class="card" id="readout" style="margin-top:12px"></div>
    <div class="btn-row" style="margin-top:16px">
      <a class="btn back" href="#/setup/2">${icon('back')}Pick your spot</a>
      <button class="btn primary" id="send" disabled>${icon('check')}Send to Pick your spot</button>
    </div>
    <p class="small muted" style="margin-top:12px">The map pictures and the address search come from OpenStreetMap's servers, which see your address search and the area you look at. Your pins only go to your own VST.</p>`.__html;

  const mapEl = $('#map'), tilesEl = $('#tiles'), svg = $('#pins');
  const tileImgs = new Map();
  const size = () => [mapEl.clientWidth, mapEl.clientHeight];
  const toScreen = ([lat, lon]) => { const [w, h] = size(); return [lonToX(lon, z) - cx + w / 2, latToY(lat, z) - cy + h / 2]; };
  const toLatLon = (sx, sy) => { const [w, h] = size(); return [yToLat(cy + sy - h / 2, z), xToLon(cx + sx - w / 2, z)]; };

  /** Lays the tiles for the current view, reusing the <img>s already there; past zoom 19 the z19 tiles are stretched. */
  const paintTiles = () => {
    const [w, h] = size();
    const tz = Math.min(z, TILE_MAX_Z), scale = 2 ** (z - tz), ts = TILE * scale, n = 2 ** tz;
    const x0 = Math.floor((cx - w / 2) / ts), x1 = Math.floor((cx + w / 2) / ts);
    const y0 = Math.max(0, Math.floor((cy - h / 2) / ts)), y1 = Math.min(n - 1, Math.floor((cy + h / 2) / ts));
    const keep = new Set();
    for (let ty = y0; ty <= y1; ty++) for (let tx = x0; tx <= x1; tx++) {
      const wx = ((tx % n) + n) % n, key = `${tz}/${wx}/${ty}`;
      keep.add(key);
      let img = tileImgs.get(key);
      if (!img) {
        img = document.createElement('img');
        img.alt = '';
        img.draggable = false;
        img.src = `${TILES}/${key}.png`;
        tilesEl.append(img);
        tileImgs.set(key, img);
      }
      Object.assign(img.style, { left: `${tx * ts - cx + w / 2}px`, top: `${ty * ts - cy + h / 2}px`, width: `${ts}px`, height: `${ts}px` });
    }
    for (const [key, img] of tileImgs) if (!keep.has(key)) { img.remove(); tileImgs.delete(key); }
  };

  const result = () => (pins.cam && pins.lane ? plan.fromMap({ camLat: pins.cam[0], camLon: pins.cam[1], laneLat: pins.lane[0], laneLon: pins.lane[1], ...(way || {}) }) : null);

  /** The two pins and the line between them, with the distance on it. */
  const paintPins = () => {
    const [w, h] = size();
    svg.setAttribute('viewBox', `0 0 ${w} ${h}`);
    let s = '';
    if (pins.cam && pins.lane) {
      const [ax, ay] = toScreen(pins.cam), [bx, by] = toScreen(pins.lane);
      const r = result(), d = plan.groundDistanceM(pins.cam[0], pins.cam[1], pins.lane[0], pins.lane[1]) * plan.FT;
      s += `<line x1="${ax}" y1="${ay}" x2="${bx}" y2="${by}" stroke="#fff" stroke-width="5" stroke-linecap="round"/><line x1="${ax}" y1="${ay}" x2="${bx}" y2="${by}" stroke="#0f766e" stroke-width="3" stroke-dasharray="6 5" stroke-linecap="round"/>`;
      s += `<text x="${(ax + bx) / 2}" y="${Math.max(ay, by) + 24}" text-anchor="middle" class="pin-label" fill="#0b0d10" stroke="#fff" stroke-width="4" paint-order="stroke">${d.toFixed(0)} ft${r ? '' : ' (too close or too far)'}</text>`;
    }
    const pin = (ll, cls, label) => {
      if (!ll) return '';
      const [x, y] = toScreen(ll);
      return `<g class="pin ${cls}${which === cls ? ' on' : ''}"><path d="M${x} ${y}c-9-12-14-18-14-25a14 14 0 0 1 28 0c0 7-5 13-14 25z"/><text x="${x}" y="${y - 21}" text-anchor="middle">${label}</text></g>`;
    };
    s += pin(pins.cam, 'cam', '1') + pin(pins.lane, 'lane', '2');
    svg.innerHTML = s;
  };

  const paintReadout = () => {
    const r = result();
    const what = { none: '', idle: '', looking: 'Looking up the street under pin 2…', found: '', missing: 'OpenStreetMap has no street right under pin 2. Move it onto the lane, or send it as it is: the distance still counts.', failed: "Couldn't reach OpenStreetMap to read the street. The distance still counts; set one-way and compass in step 4." }[wayState];
    $('#readout').innerHTML = (!pins.cam ? html`<p class="small" style="margin:0"><b>1.</b> Find your house, then tap the window the camera looks out of.</p>`
      : !pins.lane ? html`<p class="small" style="margin:0"><b>2.</b> Now tap the middle of the moving lane, straight out in front of the window.</p>`
      : !r ? html`<p class="small" style="margin:0;color:var(--warn)">The pins should be 2 to 500 ft apart: one on your window, one in the middle of the lane in front of it.</p>`
      : html`
        <div class="kpis three">
          <div class="kpi"><b>${r.distFt.toFixed(0)} ft</b><span>To the lane</span></div>
          <div class="kpi"><b>${r.facing}</b><span>Facing</span></div>
          <div class="kpi"><b>${way?.maxspeedMph || '–'}</b><span>Limit (mph)</span></div>
        </div>
        ${way ? html`<p class="small" style="margin:10px 0 0"><b>${way.street || 'An unnamed street'}</b>, ${way.oneway ? html`one-way: cars may only go <b>${r.legal === 'L2R' ? 'left to right' : 'right to left'}</b> in the picture` : 'two-way'}. Left to right in the picture is ${r.l2r}-bound, right to left ${r.r2l}-bound.</p>` : ''}
        ${what ? html`<p class="small muted" style="margin:10px 0 0">${what}</p>` : ''}
        <p class="small muted" style="margin:8px 0 0">A map knows the distance to a few feet but not what blocks the view, so the clear stretch stays as it was; sizing it from the picture or calibrating sharpens it.</p>`).__html;
    if (wayState === 'looking' && !r) $('#readout').insertAdjacentHTML('beforeend', html`<p class="small muted">${what}</p>`.__html);
    $('#send').disabled = !r;
    $('#tip').textContent = which === 'cam' ? 'Tap the map to put pin 1 on your window.' : 'Tap the map to put pin 2 in the middle of the lane.';
    $$('#which button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === which));
  };

  const paint = () => { paintTiles(); paintPins(); paintReadout(); };

  /** Asks Overpass which street is under the lane pin. */
  let lookup = 0;
  const lookUpStreet = async () => {
    if (!pins.lane) return;
    const id = ++lookup;
    wayState = 'looking'; way = null; paintReadout();
    const [lat, lon] = pins.lane;
    const q = `[out:json][timeout:15];way(around:30,${lat.toFixed(6)},${lon.toFixed(6)})[highway];out tags geom;`;
    try {
      const res = await fetch(OVERPASS, { method: 'POST', body: new URLSearchParams({ data: q }) });
      if (!res.ok) throw new Error(res.status);
      const j = await res.json();
      if (id !== lookup) return;
      way = nearestStreet(j.elements, lat, lon);
      wayState = way ? 'found' : 'missing';
    } catch { if (id === lookup) wayState = 'failed'; }
    if (id === lookup) paint();
  };

  const place = (key, ll) => {
    pins[key] = ll;
    sent = false;
    ctx.leaving = leave;
    if (key === 'lane') lookUpStreet();
    else if (!pins.lane) which = 'lane';   // window placed: the lane is next
    paint();
  };
  const leave = () => (sent ? Promise.resolve(true) : confirmSheet('Leave the map?', "Your pins haven't been sent to Pick your spot yet.", 'Leave', true));

  // ---- moving the map: drag to pan (or drag a pin), pinch or wheel or +/- to zoom, tap to place ----
  const zoomAt = (nz, sx, sy) => {
    nz = Math.max(MIN_Z, Math.min(MAX_Z, nz));
    if (nz === z) return;
    const [w, h] = size();
    const [lat, lon] = toLatLon(sx ?? w / 2, sy ?? h / 2);
    z = nz;
    // Keep the point under the finger (or the centre) where it was.
    cx = lonToX(lon, z) - ((sx ?? w / 2) - w / 2);
    cy = latToY(lat, z) - ((sy ?? h / 2) - h / 2);
    paint();
  };
  $('#zin').onclick = () => zoomAt(z + 1);
  $('#zout').onclick = () => zoomAt(z - 1);
  mapEl.addEventListener('wheel', (e) => {
    e.preventDefault();
    const r = mapEl.getBoundingClientRect();
    zoomAt(z + (e.deltaY < 0 ? 1 : -1), e.clientX - r.left, e.clientY - r.top);
  }, { passive: false });
  mapEl.addEventListener('keydown', (e) => {
    const step = 80, k = { ArrowLeft: [-step, 0], ArrowRight: [step, 0], ArrowUp: [0, -step], ArrowDown: [0, step] }[e.key];
    if (k) { e.preventDefault(); cx += k[0]; cy += k[1]; paint(); }
    else if (e.key === '+' || e.key === '=') zoomAt(z + 1);
    else if (e.key === '-') zoomAt(z - 1);
  });

  const pointers = new Map();
  let gesture = null;   // { kind: 'pan'|'pin'|'pinch', ... }
  const local = (e) => { const r = mapEl.getBoundingClientRect(); return [e.clientX - r.left, e.clientY - r.top]; };
  mapEl.addEventListener('pointerdown', (e) => {
    if (e.target.closest('.map-zoom, .map-attr')) return;
    mapEl.setPointerCapture(e.pointerId);
    pointers.set(e.pointerId, local(e));
    if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      gesture = { kind: 'pinch', d0: Math.hypot(a[0] - b[0], a[1] - b[1]), z0: z };
      return;
    }
    const [sx, sy] = local(e);
    // A pin within ~28 px of the finger is dragged; anywhere else the map pans.
    const near = ['lane', 'cam'].find((k) => pins[k] && Math.hypot(toScreen(pins[k])[0] - sx, toScreen(pins[k])[1] - 14 - sy) < 28);
    gesture = { kind: near ? 'pin' : 'pan', key: near, start: [sx, sy], last: [sx, sy], moved: 0, c0: [cx, cy] };
    if (near) { which = near; paintPins(); paintReadout(); }
  });
  mapEl.addEventListener('pointermove', (e) => {
    if (!pointers.has(e.pointerId) || !gesture) return;
    pointers.set(e.pointerId, local(e));
    if (gesture.kind === 'pinch' && pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      const ratio = Math.hypot(a[0] - b[0], a[1] - b[1]) / gesture.d0;
      const want = gesture.z0 + Math.round(Math.log2(ratio));
      if (want !== z) zoomAt(want, (a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
      return;
    }
    const [sx, sy] = local(e);
    gesture.moved = Math.max(gesture.moved, Math.hypot(sx - gesture.start[0], sy - gesture.start[1]));
    if (gesture.kind === 'pin') {
      pins[gesture.key] = toLatLon(sx, sy + 14);
      sent = false; ctx.leaving = leave;
      paintPins(); paintReadout();
    } else {
      cx = gesture.c0[0] - (sx - gesture.start[0]);
      cy = gesture.c0[1] - (sy - gesture.start[1]);
      paintTiles(); paintPins();
    }
  });
  const end = (e) => {
    if (!pointers.delete(e.pointerId) || !gesture) return;
    const g = gesture;
    if (pointers.size) return;   // the other finger of a pinch is still down
    gesture = null;
    if (g.kind === 'pin') { if (g.key === 'lane') lookUpStreet(); paint(); return; }
    // A tap (no real movement) places the selected pin.
    if (g.kind === 'pan' && g.moved < 6) place(which, toLatLon(...g.start));
  };
  mapEl.addEventListener('pointerup', end);
  mapEl.addEventListener('pointercancel', (e) => { pointers.delete(e.pointerId); gesture = null; });

  $('#which').onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v) { which = v; paintPins(); paintReadout(); } };

  // ---- finding the house ----
  const goTo = (lat, lon, zoom = 19) => { z = zoom; cx = lonToX(lon, z); cy = latToY(lat, z); paint(); };
  $('#search').onsubmit = async (e) => {
    e.preventDefault();
    const q = $('#q').value.trim();
    if (!q) return;
    const list = $('#results');
    list.hidden = false;
    list.innerHTML = html`<div class="item"><span class="small muted">Searching OpenStreetMap…</span></div>`.__html;
    try {
      const res = await fetch(`${SEARCH}?${new URLSearchParams({ format: 'jsonv2', q, limit: '5', addressdetails: '0' })}`, { headers: { Accept: 'application/json' } });
      const hits = res.ok ? await res.json() : [];
      if (!hits.length) { list.innerHTML = html`<div class="item"><span class="small">Nothing found. Try the street and town, e.g. "7½ Street SW, Charlottesville".</span></div>`.__html; return; }
      list.innerHTML = hits.map((h, i) => html`<button class="item" data-i="${i}" style="width:100%;text-align:left"><span class="small">${h.display_name}</span></button>`.__html).join('');
      list.onclick = (ev) => {
        const h = hits[Number(ev.target.closest('[data-i]')?.dataset.i)];
        if (!h) return;
        list.hidden = true;
        goTo(Number(h.lat), Number(h.lon));
        toast('Now tap your window on the map.');
      };
      if (hits.length === 1) list.querySelector('[data-i]').click();
    } catch {
      list.innerHTML = html`<div class="item"><span class="small">Couldn't reach OpenStreetMap's search. Check the internet connection, or drag and zoom the map to your street.</span></div>`.__html;
    }
  };
  $('#locate')?.addEventListener('click', () => navigator.geolocation.getCurrentPosition(
    (pos) => goTo(pos.coords.latitude, pos.coords.longitude),
    () => toast("Couldn't get this device's location. Search for the address instead."),
    { enableHighAccuracy: true, timeout: 10e3 }));

  // ---- sending ----
  $('#send').onclick = async () => {
    const m = { camLat: pins.cam[0], camLon: pins.cam[1], laneLat: pins.lane[0], laneLon: pins.lane[1] };
    if (way) Object.assign(m, { wayBearing: way.wayBearing, oneway: way.oneway, ...(way.maxspeedMph ? { maxspeedMph: way.maxspeedMph } : {}), street: way.street });
    const r = await vst.putPlan({ map: m });
    if (!r.ok) { toast(`${r.problems.join('. ')}.`); return; }
    sent = true;
    ctx.leaving = null;
    toast(r.plan.autosave ? 'Sent and saved.' : 'Sent. It is a preview until you press Save on Pick your spot.');
    ctx.go('#/setup/2');
  };

  const onResize = () => paint();
  window.addEventListener('resize', onResize);
  paint();
  if (pins.lane && wayState === 'idle') lookUpStreet();
  return () => { window.removeEventListener('resize', onResize); ctx.leaving = null; };
}
