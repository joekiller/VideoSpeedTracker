/**
 * Touch tools for editing things drawn over the camera picture on a phone: a zoomable, pannable stage with a loupe,
 * a hold-to-repeat nudge pad, and the car-length ruler for "Size it from the picture".
 *
 * Why: on a 390 px wide phone the 1280x720 frame is about 220 px tall, so one CSS pixel is 3.3 frame pixels and a
 * fingertip covers ~40 px of frame, exactly where the user is trying to look. Three things fix that, and every
 * editor here gets all three:
 *
 *   1. Zoom.   Pinch (two fingers), double-tap, or the + / − / fit buttons; up to 6x. One finger on an empty spot
 *              pans while zoomed. Handles and strokes are redrawn at a constant on-screen size (the `k` passed to
 *              `draw` is frame units per CSS pixel at the current zoom).
 *   2. Offset drags. A drag moves the grabbed thing by how far the finger moves, not to where the finger is, so
 *              the user can grab beside a handle and still see it. While dragging, a loupe in the opposite top corner
 *              shows the area around the handle magnified with a crosshair.
 *   3. Nudges. Arrow buttons move the selected thing one frame pixel per tap; holding repeats and speeds up.
 *
 * Coordinates handed to callers are always frame coordinates (0..W, 0..H), so the overlay maths in overlay.js does not
 * care about zoom. Frame coordinates come from the SVG's bounding box, which already includes the CSS transform.
 */
import { html, $, icon } from './ui.js';
import { W, H } from './overlay.js';

const clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
export const MAX_ZOOM = 6;

/**
 * Pan/zoom maths, kept pure so it can be unit tested. A view is { z, tx, ty }: the stage is scaled by z about its
 * top-left corner, then shifted by (tx, ty) CSS px. `w`, `h` are the editor's CSS size.
 *
 * zoomAbout keeps the stage point under (px, py) fixed while the zoom changes, then clamps so no black edge shows.
 */
export function zoomAbout(view, z, px, py, w, h) {
  const nz = clamp(z, 1, MAX_ZOOM);
  const sx = (px - view.tx) / view.z, sy = (py - view.ty) / view.z;   // stage point under the finger
  return clampView({ z: nz, tx: px - sx * nz, ty: py - sy * nz }, w, h);
}

/** Keeps the scaled stage covering the editor: tx in [w - w*z, 0], likewise ty. */
export function clampView(v, w, h) {
  return { z: v.z, tx: clamp(v.tx, w - w * v.z, 0), ty: clamp(v.ty, h - h * v.z, 0) };
}

/**
 * Wraps `editor` (an .editor element holding the picture and an SVG with viewBox 0 0 W H) in a pan/zoom stage.
 *
 * Options:
 *   draw(k)            redraw the overlay; k = frame units per CSS px at the current zoom.
 *   press(pt)          a finger went down at frame point pt. Return a drag { at: [x, y], move([x, y]), end() } to take
 *                      the gesture (the stage then calls move with at + finger travel), or null to let it pan/tap.
 *   tap(pt)            a quick tap that no press() claimed.
 *   loupe              true to show the loupe while a drag is live (needs an <img> in the editor; video can't be cloned
 *                      into SVG, so the cones editor turns it off).
 *   bar                markup for the left of the zoom bar under the picture (default: a one-line gesture hint).
 *
 * Returns { k(), redraw(), zoomTo(pt, z), reset(), destroy() }.
 */
export function frameStage(editor, { draw, press = () => null, tap = () => {}, loupe = true, bar = null } = {}) {
  // Move the picture and overlay into an inner stage that we transform; controls and loupe stay outside it.
  const stage = document.createElement('div');
  stage.className = 'zoom-stage';
  stage.append(...editor.childNodes);
  editor.append(stage);
  editor.classList.add('zoomable');
  // The zoom buttons go in a bar under the picture, not on it: any corner of the picture is where a box corner handle
  // ends up sooner or later, and a button over it would swallow the touch.
  editor.insertAdjacentHTML('beforeend', html`
    <div class="zoom-badge" aria-hidden="true"></div>
    <div class="loupe" hidden aria-hidden="true"><svg viewBox="0 0 10 10" preserveAspectRatio="xMidYMid slice"></svg></div>`.__html);
  editor.insertAdjacentHTML('afterend', html`
    <div class="zoom-ctl" role="group" aria-label="Zoom the picture">
      ${bar || html`<span class="small muted">Pinch, double-tap or</span>`}
      <button type="button" data-z="out" aria-label="Zoom out">−</button>
      <button type="button" data-z="in" aria-label="Zoom in">+</button>
      <button type="button" data-z="fit" aria-label="Show the whole picture">${icon('fit')}</button>
    </div>`.__html);
  const svg = stage.querySelector('svg');
  const ctl = editor.nextElementSibling, badge = editor.querySelector('.zoom-badge');
  const lp = editor.querySelector('.loupe'), lsvg = lp.querySelector('svg');

  let view = { z: 1, tx: 0, ty: 0 };
  const size = () => [editor.clientWidth || 360, editor.clientHeight || 202];
  const k = () => W / (size()[0] * view.z);
  const apply = () => {
    stage.style.transform = `translate(${view.tx}px, ${view.ty}px) scale(${view.z})`;
    $('[data-z="fit"]', ctl).disabled = view.z <= 1.01;
    $('[data-z="out"]', ctl).disabled = view.z <= 1.01;
    $('[data-z="in"]', ctl).disabled = view.z >= MAX_ZOOM - 0.01;
    badge.textContent = view.z > 1.01 ? `${view.z.toFixed(1)}×` : '';
    draw(k());
  };
  const setView = (v) => { view = clampView(v, ...size()); apply(); };
  /** Zooms so frame point pt sits in the middle of the editor at zoom z. */
  const zoomTo = ([fx, fy], z) => {
    const [w, h] = size(), nz = clamp(z, 1, MAX_ZOOM);
    setView({ z: nz, tx: w / 2 - fx / W * w * nz, ty: h / 2 - fy / H * h * nz });
  };
  /** Client px to frame units, using the SVG's on-screen box (which includes the transform). */
  const toFrame = (cx, cy) => {
    const r = svg.getBoundingClientRect();
    return [(cx - r.left) / r.width * W, (cy - r.top) / r.height * H];
  };
  const local = (cx, cy) => { const r = editor.getBoundingClientRect(); return [cx - r.left, cy - r.top]; };

  ctl.addEventListener('click', (e) => {
    const b = e.target.closest('[data-z]');
    if (!b) return;
    const [w, h] = size();
    if (b.dataset.z === 'fit') setView({ z: 1, tx: 0, ty: 0 });
    else setView(zoomAbout(view, b.dataset.z === 'in' ? view.z * 1.6 : view.z / 1.6, w / 2, h / 2, w, h));
  });

  /** The loupe: the area around frame point pt, 3x the current on-screen scale, in the corner away from the finger. */
  const showLoupe = (pt, fingerX) => {
    if (!loupe) return;
    const img = stage.querySelector('img');
    const span = 120 * k() / 3;   // frame units across the 120 px loupe
    lsvg.setAttribute('viewBox', `${pt[0] - span / 2} ${pt[1] - span / 2} ${span} ${span}`);
    lsvg.innerHTML = (img ? `<image href="${img.currentSrc || img.src}" width="${W}" height="${H}" preserveAspectRatio="none"/>` : '')
      // Lines only, no handles: the handle would fill the loupe and hide the very line being placed.
      + [...svg.children].filter((n) => !n.classList.contains('handle')).map((n) => n.outerHTML).join('')
      + `<path d="M${pt[0] - span / 2} ${pt[1]}H${pt[0] + span / 2}M${pt[0]} ${pt[1] - span / 2}V${pt[1] + span / 2}" stroke="#fff" stroke-width="${span / 120}" stroke-dasharray="${span / 40}" opacity=".9"/>`;
    lp.classList.toggle('left', fingerX > size()[0] / 2);
    lp.hidden = false;
  };

  // ---- gestures ----
  const pts = new Map();   // pointerId -> [clientX, clientY]
  let mode = null;         // 'drag' | 'pan' | 'pinch' | 'tap'
  let drag = null, start = null, pinch0 = null, lastTap = 0;

  const begin = (e) => {
    if (e.button > 0) return;
    pts.set(e.pointerId, [e.clientX, e.clientY]);
    editor.setPointerCapture?.(e.pointerId);
    e.preventDefault();
    if (pts.size === 2) {
      // A second finger always means zoom: drop whatever the first finger was doing (a drag keeps where it got to).
      drag?.end?.(); drag = null; lp.hidden = true;
      const [a, b] = [...pts.values()];
      const [mx, my] = local((a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
      pinch0 = { d: Math.hypot(a[0] - b[0], a[1] - b[1]) || 1, view: { ...view }, mx, my };
      mode = 'pinch';
      return;
    }
    if (pts.size > 2) return;
    const pt = toFrame(e.clientX, e.clientY);
    start = { cx: e.clientX, cy: e.clientY, pt, view: { ...view } };
    drag = press(pt);
    mode = drag ? 'drag' : 'tap';
    if (drag) showLoupe(drag.at, local(e.clientX, e.clientY)[0]);
  };
  const move = (e) => {
    if (!pts.has(e.pointerId)) return;
    pts.set(e.pointerId, [e.clientX, e.clientY]);
    if (mode === 'pinch' && pts.size >= 2) {
      const [a, b] = [...pts.values()];
      const d = Math.hypot(a[0] - b[0], a[1] - b[1]);
      const [mx, my] = local((a[0] + b[0]) / 2, (a[1] + b[1]) / 2);
      // Zoom about where the fingers started, then follow the midpoint so two fingers also pan.
      const v = zoomAbout(pinch0.view, pinch0.view.z * d / pinch0.d, pinch0.mx, pinch0.my, ...size());
      setView({ z: v.z, tx: v.tx + mx - pinch0.mx, ty: v.ty + my - pinch0.my });
      return;
    }
    const dx = e.clientX - start.cx, dy = e.clientY - start.cy;
    if (mode === 'drag') {
      // Finger travel in frame units: the SVG box already reflects the zoom.
      const r = svg.getBoundingClientRect();
      const at = [drag.at[0] + dx / r.width * W, drag.at[1] + dy / r.height * H];
      const shown = drag.move(at) || at;
      showLoupe(shown, local(e.clientX, e.clientY)[0]);
    } else if (mode === 'tap' && Math.hypot(dx, dy) > 8) {
      // Zoomed: one finger pans the picture. Not zoomed: it scrolls the page, as anywhere else on it, so the
      // picture never traps a thumb that is just scrolling past.
      mode = view.z > 1.01 ? 'pan' : 'scroll';
      start.sy = e.clientY;
    }
    if (mode === 'pan') setView({ z: view.z, tx: start.view.tx + dx, ty: start.view.ty + dy });
    else if (mode === 'scroll') { window.scrollBy(0, start.sy - e.clientY); start.sy = e.clientY; }
  };
  const finish = (e) => {
    if (!pts.has(e.pointerId)) return;
    pts.delete(e.pointerId);
    if (mode === 'drag') { drag.end?.(); drag = null; lp.hidden = true; }
    if (mode === 'tap' && e.type === 'pointerup') {
      const now = Date.now();
      if (now - lastTap < 300) {
        // Double tap: zoom in on the spot, or back out if already zoomed.
        const [lx, ly] = local(e.clientX, e.clientY);
        setView(view.z > 1.01 ? { z: 1, tx: 0, ty: 0 } : zoomAbout(view, 2.5, lx, ly, ...size()));
        lastTap = 0;
      } else { lastTap = now; tap(start.pt); }
    }
    if (pts.size === 0) mode = null;
    else if (mode === 'pinch' && pts.size === 1) {
      // One finger left after a pinch: carry on panning from where it is, never start a drag mid-gesture.
      const [[cx, cy]] = [...pts.values()];
      start = { cx, cy, pt: toFrame(cx, cy), view: { ...view } };
      mode = 'pan';
    }
  };
  editor.addEventListener('pointerdown', begin);
  editor.addEventListener('pointermove', move);
  editor.addEventListener('pointerup', finish);
  editor.addEventListener('pointercancel', finish);
  // Safari's own gesture events would zoom the whole page; the stage does the zooming.
  const stopGesture = (e) => e.preventDefault();
  editor.addEventListener('gesturestart', stopGesture);

  const onResize = () => setView(view);
  window.addEventListener('resize', onResize);
  requestAnimationFrame(apply);
  return {
    k, zoomTo, toFrame,
    get zoom() { return view.z; },
    redraw: () => draw(k()),
    reset: () => setView({ z: 1, tx: 0, ty: 0 }),
    destroy: () => { window.removeEventListener('resize', onResize); ctl.remove(); },
  };
}

/**
 * A pad of arrow buttons. Each tap calls onNudge(dx, dy) with a step of 1; holding repeats after 350 ms and goes up
 * to 4 per tick after a second, so crossing the frame takes a few seconds, not a minute.
 * `axes` is 'xy', 'x' or 'y'; the unused arrows are disabled. Returns { setAxes(axes) }.
 */
export function nudgePad(el, onNudge, axes = 'xy') {
  el.classList.add('nudge');
  el.innerHTML = html`
    <button type="button" data-d="-1,0" aria-label="Move left">${icon('back')}</button>
    <button type="button" data-d="0,-1" aria-label="Move up">${icon('up')}</button>
    <button type="button" data-d="0,1" aria-label="Move down">${icon('down')}</button>
    <button type="button" data-d="1,0" aria-label="Move right">${icon('next')}</button>`.__html;
  let timer = 0;
  const stop = () => { clearTimeout(timer); timer = 0; };
  el.addEventListener('pointerdown', (e) => {
    const b = e.target.closest('[data-d]');
    if (!b || b.disabled) return;
    e.preventDefault();
    const [dx, dy] = b.dataset.d.split(',').map(Number);
    const t0 = Date.now();
    onNudge(dx, dy);
    const tick = () => { const s = Date.now() - t0 > 1000 ? 4 : 1; onNudge(dx * s, dy * s); timer = setTimeout(tick, 50); };
    timer = setTimeout(tick, 350);
  });
  for (const ev of ['pointerup', 'pointercancel', 'pointerleave']) el.addEventListener(ev, stop);
  // Keyboard and screen readers: a click without a pointer (Enter/Space) nudges once.
  el.addEventListener('click', (e) => {
    const b = e.target.closest('[data-d]');
    if (b && e.detail === 0) { const [dx, dy] = b.dataset.d.split(',').map(Number); onNudge(dx, dy); }
  });
  const setAxes = (a) => {
    for (const b of el.querySelectorAll('[data-d]')) {
      const [dx] = b.dataset.d.split(',').map(Number);
      b.disabled = dx ? !a.includes('x') : !a.includes('y');
    }
  };
  setAxes(axes);
  return { setAxes };
}

// ---- the car-length ruler ---------------------------------------------------------------------------------------

/**
 * The ruler's geometry, pure for tests. n = car lengths across the 1280 px frame, so one block is W / n wide.
 * The measuring block starts at x and is centred on y; the other blocks repeat it across the frame either side.
 */
export const RULER_MIN = 1, RULER_MAX = 20, RULER_EDGE = 20;
export const blockPx = (n) => W / n;
export const nFromBlock = (px) => clamp(W / Math.max(1, px), RULER_MIN, RULER_MAX);

/** Blocks [{x, w, main}] covering the frame, phase-locked to the measuring block at x. */
export function rulerBlocks(n, x) {
  const w = blockPx(n), out = [];
  for (let bx = x - Math.ceil(x / w) * w; bx < W; bx += w) out.push({ x: bx, w, main: Math.abs(bx - x) < 0.5 });
  return out;
}

/**
 * Moves one end of the measuring block. The other end stays put; the length is kept between 1 and 20 car lengths.
 * Returns the new { n, x }.
 */
export function dragRulerEnd(n, x, end, fx) {
  const w = blockPx(n), right = x + w;
  const minW = W / RULER_MAX, maxW = W / RULER_MIN;
  if (end === 'right') return { n: nFromBlock(clamp(fx - x, minW, maxW)), x };
  const nw = clamp(right - fx, minW, maxW);
  return { n: nFromBlock(nw), x: right - nw };
}

/**
 * "Size it from the picture": a still from the camera with a striped car-length ruler over it. The bright block is
 * the one to line up with a car: drag it over the car (sideways and up or down), then drag its two ends to the
 * bumpers. Pinch or + to zoom in on the car, and the arrow pads move it or change its length a pixel at a time.
 * "New picture" grabs a fresh still, so the user can tap it as a car drives past and line up on that.
 *
 * The offset is only for lining up: what VST gets is the car-length count, handed to onUse(n) when the user taps
 * "Use it". onChange(n, [x, y]) fires on every change, so the caller can keep the block where it is across a re-render
 * (pass it back as `at`).
 *
 * `el` is an empty container; `frameUrl()` returns a fresh still's URL; `carFt` is VST's typical car length.
 */
export function carRuler(el, { frameUrl, n: n0 = 4, at = null, carFt = 15.5, onUse = () => {}, onChange = () => {} }) {
  let n = clamp(n0, RULER_MIN, RULER_MAX);
  // Starts centred in the picture at about wheel height, unless the caller kept where the user left it.
  let [x, y] = at || [W / 2 - blockPx(n) / 2, 397];
  let part = 'move';   // what the nudge pad moves: 'move' (the block) or 'length'
  el.innerHTML = html`
    <div class="editor ruler" id="ruler-ed"><img src="${frameUrl()}" alt="Camera picture to measure a car on"><svg viewBox="0 0 ${W} ${H}"></svg></div>
    <p class="small muted" style="margin:8px 0 0">Drag the bright block onto a car, then its ends to the bumpers. Zoom in to get them exact.</p>
    <div class="ruler-tools">
      <div class="seg" id="ruler-part" role="group" aria-label="What the arrows change">
        <button type="button" data-v="move" aria-pressed="true">Move</button>
        <button type="button" data-v="length" aria-pressed="false">Length</button>
      </div>
      <div id="ruler-pad"></div>
    </div>
    <label class="field" style="margin-top:12px"><span id="ruler-label"></span>
      <input type="range" id="ruler-n" min="${RULER_MIN}" max="${RULER_MAX}" step="0.05" aria-describedby="ruler-help"></label>
    <p class="small muted" id="ruler-help" style="margin:6px 0 10px">One block is one car. A car parked along the street is easiest; a car driving past is best.</p>
    <button type="button" class="btn block" id="ruler-use">${icon('ruler')}<span id="ruler-use-text"></span></button>`.__html;

  const ed = $('#ruler-ed', el), svg = $('svg', ed);
  let active = null;   // 'body' | 'left' | 'right' while dragging, for the highlight

  /**
   * The band is drawn in screen px (k = frame units per CSS px), not frame px, so zooming in on a car makes the
   * band no thicker: it stays a thin strip with tall end ticks, and the car's bumpers stay visible either side.
   */
  const draw = (k) => {
    const hh = 7 * k, tick = 26 * k;
    let s = '';
    for (const b of rulerBlocks(n, x)) {
      const col = b.main ? '#f5a524' : '#22d3ee';
      s += `<rect x="${b.x}" y="${y - hh}" width="${b.w}" height="${2 * hh}" fill="${col}" fill-opacity="${b.main ? 0.6 : 0.3}"/>`
        + `<line x1="${b.x}" x2="${b.x}" y1="${y - tick}" y2="${y + tick}" stroke="${col}" stroke-opacity="${b.main ? 1 : 0.6}" stroke-width="${2 * k}"/>`;
    }
    const w = blockPx(n);
    s += `<rect x="${x}" y="${y - hh}" width="${w}" height="${2 * hh}" fill="none" stroke="#fff" stroke-width="${1.5 * k}"/>`;
    // End handles: pills on the bright block's ends, sized in screen px so they stay grabbable when zoomed. They sit
    // below the band so the end tick itself stays visible against the bumper.
    for (const [id, hx] of [['left', x], ['right', x + w]]) {
      const r = 9 * k, on = active === id, cy = y + tick + r * 1.2;
      s += `<line x1="${hx}" x2="${hx}" y1="${y - tick}" y2="${cy}" stroke="#fff" stroke-width="${2 * k}"/>`
        + `<g class="handle" data-h="${id}"><rect x="${hx - r * 0.7}" y="${cy - r * 1.3}" width="${r * 1.4}" height="${r * 2.6}" rx="${r * 0.6}" fill="${on ? '#f5a524' : '#fff'}" stroke="#0b0d10" stroke-width="${2 * k}"/></g>`;
    }
    svg.innerHTML = s;
  };

  const st = frameStage(ed, {
    draw,
    // A new still to measure on: tap it as a car drives past, then line up on that car.
    bar: html`<button type="button" class="btn small" id="ruler-snap">${icon('camera')}New picture</button>`,
    press: ([fx, fy]) => {
      // Grab an end if the finger is within ~28 screen px of it, otherwise the whole block moves. On a short block the
      // ends get only the outer 30% each, so its middle always moves it.
      const k = st.k(), w = blockPx(n), near = Math.min(28 * k, w * 0.3);
      const dl = Math.abs(fx - x), dr = Math.abs(fx - (x + w));
      const end = fy > y - 40 * k && fy < y + 70 * k && Math.min(dl, dr) < near ? (dl < dr ? 'left' : 'right') : null;
      if (end) {
        active = end;
        return {
          at: [end === 'left' ? x : x + w, y],
          move: ([mx]) => { ({ n, x } = dragRulerEnd(n, x, end, mx)); changed(); return [end === 'left' ? x : x + blockPx(n), y]; },
          end: () => { active = null; st.redraw(); },
        };
      }
      // Anywhere on the striped band (all blocks move together): within ~50 screen px of it. Anywhere else scrolls
      // the page, or pans when zoomed.
      if (Math.abs(fy - y) > 40 * k) return null;
      active = 'body';
      const at = [x, y];
      return {
        at,
        move: ([mx, my]) => { x = clamp(mx, -blockPx(n) + 20, W - 20); y = clamp(my, RULER_EDGE, H - RULER_EDGE); changed(); return [x, y]; },
        end: () => { active = null; st.redraw(); },
      };
    },
  });

  const range = $('#ruler-n', el);
  const changed = (fromRange = false) => {
    $('#ruler-label', el).textContent = `${n.toFixed(1)} car lengths across`;
    $('#ruler-use-text', el).textContent = `Use it: about ${(n * carFt).toFixed(0)} ft across`;
    if (!fromRange) range.value = n;
    st.redraw();
    onChange(n, [x, y]);
  };
  range.oninput = () => {
    // Keep the bright block's middle where it is, so a slide grows it about the car it is on.
    const mid = x + blockPx(n) / 2;
    n = clamp(Number(range.value), RULER_MIN, RULER_MAX);
    x = mid - blockPx(n) / 2;
    changed(true);
  };

  nudgePad($('#ruler-pad', el), (dx, dy) => {
    if (part === 'move') { x = clamp(x + dx, -blockPx(n) + 20, W - 20); y = clamp(y + dy, RULER_EDGE, H - RULER_EDGE); }
    else ({ n, x } = dragRulerEnd(n, x, 'right', x + blockPx(n) + dx));
    changed();
  });
  const pad = $('#ruler-pad', el);
  $('#ruler-part', el).onclick = (e) => {
    const v = e.target.closest('[data-v]')?.dataset.v;
    if (!v) return;
    part = v;
    for (const b of $('#ruler-part', el).children) b.setAttribute('aria-pressed', b.dataset.v === v);
    // Length only changes sideways: the left end stays, the right end moves.
    pad.querySelectorAll('[data-d]').forEach((b) => { b.disabled = v === 'length' && b.dataset.d.startsWith('0'); });
    for (const b of pad.querySelectorAll('[data-d]')) {
      const left = b.dataset.d === '-1,0', right = b.dataset.d === '1,0';
      if (left) b.setAttribute('aria-label', v === 'length' ? 'Shorter' : 'Move left');
      if (right) b.setAttribute('aria-label', v === 'length' ? 'Longer' : 'Move right');
    }
    pad.classList.toggle('length', v === 'length');
  };
  $('#ruler-snap', ed.parentElement).onclick = () => { $('img', ed).src = frameUrl(); };
  $('#ruler-use', el).onclick = () => onUse(Math.round(n * 100) / 100);
  changed();
  return { destroy: st.destroy, get n() { return n; } };
}
