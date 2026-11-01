/**
 * Setup: the same five steps as VST Studio, numbered the same, so "step 4" means one thing on the phone and on the
 * computer.  A first-time user can follow it from the phone while standing at the window (or out on the street for
 * the calibration drive):
 *
 *   1. What you need       - the plan in five lines, and cameras that work (the phone already in a pocket is one);
 *   2. Pick your spot      - VST Studio's site planner (GET/PUT /api/plan): a preset, the map (#/map), a car-length
 *                            ruler over a still from the camera, or measured numbers.  Every change is a preview on
 *                            the computer's planner as well until someone presses Save (or turns on autosave), the
 *                            same draft Studio's own page edits, so the two never disagree about what is saved;
 *   3. Connect the camera  - what the camera sees right now and how to aim it, with a phone-as-webcam guide;
 *   4. Mark your street    - drag the analysis box, speed lines, lane line(s) and obstruction on the frame, then the
 *                            rules of the road (which way is legal, compass, limits, a name).  One Save for the page,
 *                            through PUT /api/config; VST validates and the problems it reports are shown in words.
 *                            Leaving with unsaved changes asks first (ctx.leaving, honoured by app.js's router);
 *   5. Calibrate           - drive through at a known speed, tap your pass, then drive once more to check it; or, with
 *                            no car handy, tap two spots a measured distance apart (cones, or one pole moved between).
 *
 * Step 5 uses POST /api/calibration/apply with the chosen pass, then POST /api/calibration/check with a second one.
 */
import { html, $, $$, icon, sign, band, arrow, clock, ago, toast, sheet, confirmSheet } from '../ui.js';
import { zonesSvg, applyHandle, handlePoint, HANDLE_AXES, geometry, oneLane, W, H, LANE, PARK, parkYAt } from '../overlay.js';
import { frameStage, nudgePad, carRuler } from '../touch.js';

export const STEPS = ['What you need', 'Pick your spot', 'Connect the camera', 'Mark your street', 'Calibrate'];
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

export async function render(ctx, step = '1') {
  const n = Math.min(STEPS.length, Math.max(1, Number(step) || 1));
  const cfg = await ctx.vst.config();
  const st = await ctx.vst.status();
  ctx.view.innerHTML = html`
    <div class="step-kicker">Step ${n} of ${STEPS.length} · ${STEPS[n - 1]}</div>
    <nav class="steps" aria-label="Setup steps">${STEPS.map((s, i) => html`<a href="#/setup/${i + 1}" title="${i + 1}. ${s}" aria-label="Step ${i + 1}: ${s}" ${i + 1 === n ? 'aria-current="step"' : ''} class="${i + 1 < n ? 'done' : i + 1 === n ? 'current' : ''}"></a>`)}</nav>
    <div id="step"></div>`.__html;
  const body = $('#step');
  // guard(fn) sets the app's leave check (app.js asks fn before following any link off this page).
  return [need, spot, connect, mark, drive][n - 1]({ ...ctx, body, cfg, st, n, guard: (fn) => { ctx.leaving = fn; } });
}

/** Prev/next footer shared by steps; the Next button names the step it goes to. */
const nav = (n, nextLabel) => html`
  <div class="btn-row" style="margin-top:20px">
    ${n > 1 ? html`<a class="btn back" href="#/setup/${n - 1}">${icon('back')}Back</a>` : ''}
    ${n < STEPS.length ? html`<a class="btn primary" href="#/setup/${n + 1}" id="next">${nextLabel || `Next: ${STEPS[n]}`}${icon('next')}</a>` : html`<a class="btn primary" href="#/live">Done${icon('check')}</a>`}
  </div>`;

// ---- 1. what you need ---------------------------------------------------------------------------

/** The cameras VST Studio's step 1 lists (src/Gui/Guide.cpp kTiers), cheapest first; any of them is a fine start. */
const TIERS = [
  ['Your phone, as a webcam', '$0', 'already in your pocket', 'Live, at 30 fps. The easiest start.'],
  ['Your phone, recording at 60 fps', '$0', 'free Blackmagic Camera app', 'Record, then open the video in VST Studio. Fast shutter, great timing.'],
  ['Logitech C922 webcam', 'about $70–100', 'Best Buy, Walmart, Staples', '60 fps at 720p; VST sets its shutter for you.'],
  ['Raspberry Pi 5 + Camera Module 3', 'about $150', 'online or Micro Center', 'Always on, 60 fps, sips power. For later.'],
];

function need({ body, n }) {
  const row = (i, ic, title, text) => html`<a class="check" href="#/setup/${i}" style="color:inherit;text-decoration:none">${icon(ic)}<p><b>${i}. ${STEPS[i - 1]}: ${title}</b> ${text}</p></a>`;
  body.innerHTML = html`
    <h1>Measure the traffic on your street</h1>
    <p class="lede">VST times every car between two lines on your video and works out its speed. Setting up takes about half an hour, in the same five steps as VST Studio on the computer. Every step has a way through if something isn't to hand yet.</p>
    <div class="card">
      ${row(2, 'compass', 'a window on the street.', 'Find it on the map, or pick the closest kind of street. About 14 ft of clear street is enough, and a rough guess is fine to start.')}
      ${row(3, 'camera', 'a camera, held still.', 'Your phone, a webcam, an IP or Raspberry Pi camera, on a window clamp or a stack of books.')}
      ${row(4, 'ruler', 'a few minutes with the picture.', 'Drag a box around the moving lane and two lines to time between, and say which way a one-way street runs.')}
      ${row(5, 'car', 'one drive past, or no car at all.', 'Drive past at a known speed, or use two cones or a pole.')}
    </div>
    <h3>Cameras that work</h3>
    <div class="list">${TIERS.map(([name, price, where, why]) => html`
      <div class="item"><div class="label"><b>${name}</b><small>${why}</small></div><div class="value">${price}<br><small class="muted">${where}</small></div></div>`)}
    </div>
    <p class="small muted" style="margin-top:8px">Start with what you have. Pick your spot shows how each one does on your street.</p>
    ${nav(n)}`.__html;
}

// ---- 2. pick your spot --------------------------------------------------------------------------

function spot({ vst, body, n }) {
  body.innerHTML = html`
    <h1>Pick your spot</h1>
    <p class="lede">Two lengths decide how well VST times your street: how far the window is from the moving lane, and how much of it you can see clear. A rough guess is fine; it gets sharper as you go.</p>
    <div class="card" id="plan"><p class="small muted">Loading the plan…</p></div>
    <p class="small muted" id="spot-note" style="margin-top:10px"></p>
    ${nav(n)}`.__html;
  return planCard(vst, $('#plan'));
}

const PILL = { guess: 'warn', typed: '', picture: '', calibration: 'ok', map: '' };
const GRADE = ['ok', 'warn', 'bad'];

/**
 * "Your street": the planner's verdict and where its numbers came from, the presets, the map, a car-length ruler over
 * its own zoomable still from the camera (touch.js carRuler: drag the bright block onto a car, its ends to the bumpers,
 * then "Use it"), the measured numbers when a calibration has them, and the two lengths for anyone who has measured.
 *
 * Every change goes to VST (PUT /api/plan) as a draft, which answers with the whole plan, so the card always shows
 * what the computer's planner shows.  While the draft differs from the saved plan the bar on top lists the changes,
 * with Discard and Save; "Save automatically" saves each change as it is made.  The plan is re-read every few seconds,
 * so a change made on the computer shows up here too.  Returns a cleanup for the router.
 */
async function planCard(vst, el) {
  let p;
  try { p = await vst.plan(); } catch { el.innerHTML = html`<p class="small muted">This VST doesn't have the site planner yet. Update VST Studio to plan from the phone.</p>`.__html; return null; }
  let n = p.carLengths ?? Math.min(20, Math.max(1, Math.round(p.street.coverFt / p.typicalCarFt * 2) / 2));
  // touch.js carRuler, rebuilt with the card; n and where the block sits survive a re-render (e.g. after "Use it").
  let ruler = null, rpos = null, busy = false;

  const send = async (change, said) => {
    busy = true;
    try {
      const r = await vst.putPlan(change);
      p = r.plan;
      if (!r.ok) { render(); $('#plan-problems', el).textContent = `${r.problems.join('. ')}.`; return; }
      render();
      if (said) toast(said);
    } finally { busy = false; }
  };

  const render = () => {
    const v = p.verdict, st = p.street;
    const dirty = !!p.dirty, changes = p.changes || [];
    el.innerHTML = html`
      ${dirty ? html`
        <div class="callout warn draftbar" role="status">${icon('warn')}<div class="grow">
          <p><b>Preview: ${changes.length} unsaved change${changes.length === 1 ? '' : 's'}</b></p>
          <ul class="small changes">${changes.map((c) => html`<li>${c}</li>`)}</ul>
          <p class="small muted">Saved plan: ${p.saved.label}. ${p.saved.text}</p>
          <div class="btn-row" style="margin-top:8px"><button class="btn" id="plan-discard">Discard</button><button class="btn primary" id="plan-save">${icon('check')}Save changes</button></div>
        </div></div>` : ''}
      <div class="row" style="gap:8px;flex-wrap:wrap;margin-top:${dirty ? 12 : 0}px"><span class="pill ${GRADE[v.grade]}">${v.label}</span><span class="pill ${PILL[st.source] ?? ''}">${st.sourceName}</span>${dirty ? '' : html`<span class="pill">${icon('check')}Saved</span>`}</div>
      <p style="margin:8px 0 4px">${v.text}</p>
      ${v.fastest ? html`<p class="small muted" style="margin:0">At ${p.camera.fps} fps this view times cars ${v.fastest}${p.camera.fps < 59 && v.fastest60 !== v.fastest ? html`; at 60 fps, ${v.fastest60}` : ''}.</p>` : ''}
      ${v.frameRate ? html`<p class="small" style="margin:6px 0 0;color:var(--warn)">${v.frameRate}</p>` : ''}
      <p class="small muted" style="margin:6px 0 0">${st.source === 'guess' ? 'A rough guess is fine to start; zero in below.' : st.note || 'As you typed them.'}
        ${st.distFt.toFixed(0)} ft to the lane, ${st.clearFt.toFixed(0)} ft clear, about ${st.coverFt.toFixed(0)} ft across the picture.</p>
      <div class="item switch-row"><div class="label"><b>Save automatically</b><small>Off: changes are a preview until you press Save, here or on the computer.</small></div>
        <label class="switch"><input type="checkbox" id="plan-auto" aria-label="Save automatically" ${p.autosave ? 'checked' : ''}><i></i></label></div>

      <h4 style="margin:16px 0 8px">Which is closest to your view?</h4>
      <div class="choice" id="presets">${p.presets.map((x) => html`<button data-k="${x.key}" aria-pressed="${st.source === 'guess' && Math.abs(st.distFt - x.distFt) < .5 && Math.abs(st.clearFt - x.clearFt) < .5}"><span><b>${x.name}</b><small>${x.hint}</small></span></button>`)}</div>

      <h4 style="margin:16px 0 8px">Find it on the map</h4>
      ${p.map ? html`<p class="small" style="margin:0 0 8px">${p.map.street || 'Pins placed'}: ${p.map.distFt.toFixed(0)} ft from your window to the lane, facing ${p.map.facing}.</p>` : ''}
      <a class="btn" href="#/map">${icon('compass')}${p.map ? 'Move the pins' : 'Drop two pins on the map'}</a>
      <p class="small muted" style="margin:6px 0 0">Your window and the lane in front of it. The map also reads the street's name, whether it's one-way and its limit.</p>

      <h4 style="margin:16px 0 8px">Size it from the picture</h4>
      ${p.picture ? html`<div id="ruler"></div>`
        : html`<div class="callout">${icon('camera')}<p>This needs a picture of your own street: pick your camera, or open a video of the street, in step 3 on the computer. The sample video isn't your street. Until then a preset or the map is a fine start.</p></div>`}

      ${p.measured ? html`
        <div class="callout ok" style="margin-top:14px">${icon('check')}<div class="grow"><p>Measured by ${p.measured.how}: ${p.measured.coverFt.toFixed(0)} ft across the picture, ${p.measured.clearFt.toFixed(0)} ft of it clear.</p>
          ${st.source !== 'calibration' ? html`<button class="btn small" id="use-measured" style="margin-top:8px">Use the measured numbers</button>` : ''}</div></div>`
      : html`<p class="small muted" style="margin:14px 0 0">Calibrating (step 5: a drive past, or cones or a pole) measures the street exactly, and this updates by itself.</p>`}

      <details style="margin-top:14px" ${el.dataset.typed ? 'open' : ''}><summary class="small">I've measured it</summary>
        <div class="row" style="gap:10px;margin-top:10px">
          <label class="field grow"><span>Window to the lane (ft)</span><input class="input" id="dist" type="number" inputmode="decimal" min="3" max="1000" value="${st.distFt.toFixed(0)}"></label>
          <label class="field grow"><span>Clear stretch (ft)</span><input class="input" id="clear" type="number" inputmode="decimal" min="3" max="1000" value="${st.clearFt.toFixed(0)}"></label>
        </div>
        <p class="small muted" style="margin:6px 0 0">3 to 1000 ft each. No tape? One easy step is about 2.5 ft, sidewalk slabs are usually 5 ft, a parked car about 15 ft.</p>
        <button class="btn" id="use-typed" style="margin-top:10px">Use these numbers</button>
      </details>
      <p class="small" id="plan-problems" style="color:var(--bad)"></p>`.__html;
    ruler?.destroy();
    ruler = null;
    if (p.picture) {
      ruler = carRuler($('#ruler', el), {
        frameUrl: () => vst.frameUrl(), n, at: rpos, carFt: p.typicalCarFt,
        onChange: (val, pos) => { n = val; rpos = pos; },
        onUse: (val) => send({ carLengths: val }, 'Sized from your picture. A calibration will measure it exactly.'),
      });
    }
    $('#presets', el).onclick = (e) => { const k = e.target.closest('[data-k]')?.dataset.k; if (k) send({ preset: k }); };
    $('#use-measured', el)?.addEventListener('click', () => send({ useMeasured: true }));
    $('#use-typed', el).onclick = () => { el.dataset.typed = '1'; send({ distFt: Number($('#dist', el).value), clearFt: Number($('#clear', el).value) }); };
    $('#plan-auto', el).onchange = (e) => send({ autosave: e.target.checked }, e.target.checked ? 'Changes now save as you make them.' : 'Changes are a preview until you press Save.');
    $('#plan-save', el)?.addEventListener('click', () => send({ save: true }, 'Saved. VST plans with these numbers now.'));
    $('#plan-discard', el)?.addEventListener('click', () => send({ discard: true }, 'Changes discarded.'));
    const note = $('#spot-note');
    if (note) note.textContent = dirty ? 'Unsaved changes stay as a preview here and on the computer until someone saves or discards them.' : '';
  };
  render();

  // Follow the computer: re-read the plan every 5 s and repaint when it changed, unless the user is mid-edit here.
  const poll = setInterval(async () => {
    if (busy || el.contains(document.activeElement) && document.activeElement.matches('input')) return;
    const q = await vst.plan().catch(() => null);
    if (q && JSON.stringify(q) !== JSON.stringify(p)) { p = q; render(); }
  }, 5000);
  return () => { clearInterval(poll); ruler?.destroy(); };
}

// ---- 3. connect the camera ----------------------------------------------------------------------

function connect({ vst, body, st, n }) {
  const src = st.source;
  body.innerHTML = html`
    <h1>Connect the camera</h1>
    <p class="lede">Pick the camera in VST Studio on the computer (its step 3). This page shows what it sees, so you can aim it from the window.</p>
    ${src ? html`
      <div class="editor"><img src="${vst.frameUrl()}" alt="What the camera sees now" id="frame"></div>
      <div class="row between" style="margin-top:8px"><span class="small muted">${src.name}${src.width ? ` · ${src.width}×${src.height}` : ''}${src.fps ? ` at ${Math.round(src.fps)} fps` : ''}</span>
        <button class="btn ghost" id="refresh">${icon('refresh')}Refresh</button></div>`
    : html`<div class="empty-state" style="padding:24px 16px">${icon('camera')}<h2>No camera yet</h2><p>On the computer, open VST Studio's step 3 and pick a camera, or open a video of your street. This page shows it as soon as it's on.</p>
        <button class="btn" id="recheck">${icon('refresh')}Check again</button></div>`}
    <h3>Aim it like this</h3>
    <div class="card">
      <div class="check">${icon('compass')}<p><b>Street across the middle.</b> Point straight across, not up or down the road, so cars move side to side through the middle third of the picture.</p></div>
      <div class="check">${icon('camera')}<p><b>Any mount that holds still.</b> A window clamp, a tripod, even a phone propped on books. If it gets bumped, VST notices and says so.</p></div>
      <div class="check">${icon('sparkle')}<p><b>Lens to the glass.</b> Take out any insect screen, press the lens against the window and put a dark cloth around it so the room doesn't reflect. Avoid pointing into the sun.</p></div>
    </div>
    <h3>Using a phone as the camera? We've got you</h3>
    <div class="card">
      <div class="check">${icon('check')}<p><b>Android 14 or later:</b> plug it into the computer by USB and pick "Webcam" in the USB notification.</p></div>
      <div class="check">${icon('check')}<p><b>iPhone with a Mac:</b> Continuity Camera shows your iPhone as a camera by itself; put it on a stand near the window.</p></div>
      <div class="check">${icon('check')}<p><b>Anything else:</b> a free webcam app (Camo, DroidCam or Iriun) makes the phone a camera over Wi-Fi or USB.</p></div>
      <div class="check">${icon('check')}<p><b>Or just record.</b> For faster cars, or with no webcam app at all: record at 60 fps and 1/1000 s (Blackmagic Camera is free), copy the video over and open it in step 3. VST times it just the same.</p></div>
      <p class="small muted" style="margin:6px 0 0">Keep it on the charger and on Do Not Disturb, and lock exposure if the app can.</p>
    </div>
    ${nav(n)}`.__html;
  $('#refresh')?.addEventListener('click', () => { $('#frame').src = vst.frameUrl(); });
  $('#recheck')?.addEventListener('click', async () => { const s = await vst.status(); if (s.source) connect({ vst, body, st: s, n }); else toast('Still no camera. Pick one in VST Studio\'s step 3.'); });
}

// ---- 4. mark your street, then the rules of the road ---------------------------------------------

const PARTS = {
  tl: 'Drag the corners so the box covers just the road, from curb to curb. VST ignores everything outside it, like trees moving in the wind.',
  br: 'Drag the corners so the box covers just the road, from curb to curb. VST ignores everything outside it, like trees moving in the wind.',
  sl: 'The two white lines are where timing starts and stops. Put them as far apart as the view stays clear and level.',
  sr: 'The two white lines are where timing starts and stops. Put them as far apart as the view stays clear and level.',
  r2l: 'Put the orange line where the wheels of right-to-left traffic touch the road.',
  l2r: 'Put the purple line where the wheels of left-to-right traffic touch the road.',
  lane: 'Put the lane line at hubcap height for cars in the lane. Anyone driving against the arrows is logged as wrong-way.',
  pkl: "Put the blue line on the middle of a parked car's wheels. Move each end so it follows the parked cars. VST uses it in step 5 to tell a parked car from a passing one.",
  pkr: "Put the blue line on the middle of a parked car's wheels. Move each end so it follows the parked cars. VST uses it in step 5 to tell a parked car from a passing one.",
  o0: 'Mark anything in front of the road, like a post or tree trunk, so VST knows vehicles disappear behind it.',
  o1: 'Mark anything in front of the road, like a post or tree trunk, so VST knows vehicles disappear behind it.',
};

/** The parts the phone can pick and move, in the order a novice sets them; names match PARTS' words. */
const PICK = [
  ['tl', 'Box corner ↖', '#fff'], ['br', 'Box corner ↘', '#fff'], ['sl', 'Start line', '#fff'], ['sr', 'End line', '#fff'],
  ['r2l', '← Wheels', LANE.R2L], ['l2r', 'Wheels →', LANE.L2R], ['pkl', 'Parking, left end', PARK], ['pkr', 'Parking, right end', PARK],
  ['o0', 'Post, left edge', '#cbd5e1'], ['o1', 'Post, right edge', '#cbd5e1'],
];

/** Tiny street illustration for the direction choices. */
const street = (legal) => {
  const arrows = { R2L: [['R2L', 26]], L2R: [['L2R', 26]], both: [['R2L', 18], ['L2R', 34]] }[legal];
  return html`<svg class="street" viewBox="0 0 88 52" aria-hidden="true">
    <rect width="88" height="52" rx="10" fill="var(--surface-3)"/><rect y="12" width="88" height="28" fill="#2a2f37"/>
    ${legal === 'both' ? html`<line x1="6" x2="82" y1="26" y2="26" stroke="#fbbf24" stroke-width="1.5" stroke-dasharray="5 4"/>` : ''}
    ${arrows.map(([d, y]) => html`<path d="${d === 'L2R' ? `M24 ${y}H62M55 ${y - 6}l7 6-7 6` : `M64 ${y}H26M33 ${y - 6}l-7 6 7 6`}" fill="none" stroke="#5eead4" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>`)}
  </svg>`;
};

/** The config keys this page edits and saves together. */
const MARK_KEYS = ['AnalysisBoxLeft', 'AnalysisBoxTop', 'AnalysisBoxWidth', 'AnalysisBoxHeight', 'speedLineLeft', 'speedLineRight', 'obstruction',
  'R2LStreetY', 'L2RStreetY', 'R2LStreetYRight', 'L2RStreetYRight', 'streetEdge', 'parkingLane', 'parkingLaneFt',
  'legalDirection', 'L2RDirection', 'R2LDirection', 'speedLimit', 'egregious', 'siteName'];

/**
 * Mark your street.  On a phone the frame is ~220 px tall, so editing works three ways, all on one selected part:
 *   - drag on the picture: grab near a handle (the nearest one within ~32 px of the finger) and it moves by the
 *     finger's travel, with a loupe showing it magnified; pinch, double-tap or + to zoom in first;
 *   - pick a part from the chips under the picture (no need to hit a 9 px dot at all);
 *   - the arrow pad moves the picked part one frame pixel per tap, faster when held.
 * touch.js does the gestures and zoom; this view only maps a handle and a frame point to config through applyHandle.
 * A one-way street has one lane line (the legal direction's; the other follows it, overlay.js oneLane), like Studio.
 *
 * Below the picture, the rules of the road edit the same draft, and one Save (in the bar that appears with the first
 * change) sends the lot.  Leaving with unsaved changes, by Next, the tabs or Back, asks first.
 */
function mark({ vst, body, cfg, st, n, guard }) {
  const copy = (c) => ({ ...c, obstruction: [...(c.obstruction || [0, 0])], ...(c.streetEdge ? { streetEdge: [...c.streetEdge] } : {}),
    ...(c.parkingLane ? { parkingLane: [...c.parkingLane] } : {}), siteName: c.siteName || '' });
  const draft = copy(cfg);
  let sel = 'tl';
  const hasPark = () => !!draft.parkingLane && draft.parkingLane[0] > 0 && draft.parkingLane[1] > 0;
  const oneWay = () => draft.legalDirection === 'L2R' || draft.legalDirection === 'R2L';
  // The lane chip and handle on a one-way street: the legal direction's line.
  const laneId = () => draft.legalDirection.toLowerCase();
  body.innerHTML = html`
    <h1>Mark your street</h1>
    <p class="lede">Drag the handles on the picture, or pick a part below and use the arrows. Pinch or tap + to zoom in for a precise fit.</p>
    ${st.source ? '' : html`<div class="callout" style="margin-bottom:10px">${icon('camera')}<p>No camera is on yet, so there's no picture to mark. Pick one in step 3 on the computer; you can set the rules of the road below meanwhile.</p></div>`}
    <div class="editor" id="ed">${st.source ? html`<img src="${vst.frameUrl()}" alt="Camera view">` : ''}<svg viewBox="0 0 ${W} ${H}" id="svg"></svg></div>
    <div class="parts" id="parts" role="group" aria-label="Part to move"></div>
    <div class="fine">
      <div class="grow"><b id="sel-name"></b><small id="sel-pos"></small></div>
      <div id="pad"></div>
    </div>
    <div class="legend" id="legend"></div>
    <div class="callout" id="hint">${icon('info')}<p>Start with the box: drag its top-left and bottom-right corners to the edges of the road.</p></div>
    <div class="list" style="margin-top:12px">
      <div class="item"><div class="label"><b>Something blocks the view</b><small>A post, tree trunk or sign in front of the road</small></div>
        <label class="switch"><input type="checkbox" id="obs" aria-label="Something blocks the view" ${draft.obstruction[1] > draft.obstruction[0] ? 'checked' : ''}><i></i></label></div>
      ${'parkingLane' in cfg ? html`<div class="item"><div class="label"><b>Cars park along this street</b><small>Mark the parked cars' wheels, so step 5 can calibrate from a parked car</small></div>
        <label class="switch"><input type="checkbox" id="park" aria-label="Cars park along this street" ${hasPark() ? 'checked' : ''}><i></i></label></div>` : ''}
    </div>
    <div id="park-more" class="card" style="margin-top:10px" hidden>
      <p class="small" id="park-est" style="margin:0"></p>
      <label class="field" style="margin-top:8px"><span>Measured across the street (ft, optional)</span>
        <input class="input" id="park-ft" type="number" inputmode="decimal" min="0" max="40" step="0.5" placeholder="Leave empty: VST works it out">
        <span class="small muted" style="font-weight:400">From a passing car's wheels to a parked car's. About half a lane plus half a parking lane.</span></label>
    </div>

    <h2 style="margin-top:28px">Rules of the road</h2>
    <p class="lede">Which way traffic may go and how fast, so VST can flag speeders and wrong-way drivers.</p>
    <h3>Which way may traffic go?</h3>
    <div class="choice" id="legal">
      <button data-v="R2L">${street('R2L')}<span><b>One-way, right to left</b><small>Traffic going left to right is flagged as wrong way</small></span></button>
      <button data-v="L2R">${street('L2R')}<span><b>One-way, left to right</b><small>Traffic going right to left is flagged as wrong way</small></span></button>
      <button data-v="both">${street('both')}<span><b>Two-way street</b><small>Speeds only, both directions</small></span></button>
    </div>
    <h3>Compass headings</h3>
    <p class="small">Used in the stats file and on cards. Which way does traffic head when it moves across your screen? The map in step 2 fills these in.</p>
    <div class="row">
      <label class="field grow"><span>Moving ← right to left</span><select class="input" id="r2l">${COMPASS.map((c) => html`<option ${c === draft.R2LDirection ? 'selected' : ''}>${c}</option>`)}</select></label>
      <label class="field grow"><span>Moving → left to right</span><select class="input" id="l2r">${COMPASS.map((c) => html`<option ${c === draft.L2RDirection ? 'selected' : ''}>${c}</option>`)}</select></label>
    </div>
    <h3>Speeds</h3>
    <div class="row">
      <div class="field grow"><span>Posted limit</span><div class="stepper" data-k="speedLimit"><button data-d="-5" aria-label="Lower the limit">−</button><output aria-live="polite"></output><button data-d="5" aria-label="Raise the limit">+</button></div></div>
      <div class="field grow"><span>"Way too fast" from</span><div class="stepper" data-k="egregious"><button data-d="-1" aria-label="Lower">−</button><output aria-live="polite"></output><button data-d="1" aria-label="Higher">+</button></div></div>
    </div>
    <h3>Name for this spot</h3>
    <label class="field"><input class="input" id="site" aria-label="Name for this spot" maxlength="40" placeholder="e.g. 7½ St SW" value="${draft.siteName}"><span class="small muted" style="font-weight:400">Shown in the app, and on shared cards only if you allow it.</span></label>

    <div class="savebar" id="savebar" hidden role="status">
      <span class="grow"><b>Unsaved changes</b><small class="muted" id="problems"></small></span>
      <button class="btn" id="reset">Undo</button><button class="btn primary" id="save">${icon('check')}Save</button>
    </div>
    ${nav(n)}`.__html;

  const dirty = () => MARK_KEYS.some((k) => JSON.stringify(draft[k] ?? null) !== JSON.stringify(k === 'siteName' ? cfg[k] || '' : cfg[k] ?? null));
  const paintDirty = () => {
    const d = dirty();
    $('#savebar').hidden = !d;
    if (!d) $('#problems').textContent = '';
    guard(d ? leave : null);
  };
  /** Asks before leaving with unsaved changes: Save and go, leave without saving, or stay. */
  const leave = async () => {
    const choice = await sheet(html`
      <h2>Save your changes first?</h2>
      <p>You've changed the zones or the rules on this page and haven't saved them.</p>
      <div class="btn-row"><button class="btn" data-v="stay">Stay</button><button class="btn" data-v="leave">Don't save</button><button class="btn primary" data-v="save">Save</button></div>`,
    (el, close) => $$('[data-v]', el).forEach((b) => b.onclick = () => close(b.dataset.v)));
    if (choice === 'save') return save();
    return choice === 'leave';
  };
  const hasObs = () => draft.obstruction[1] > draft.obstruction[0];
  /** The chips: one lane on a one-way street (named "Lane", in the legal direction's colour), two on a two-way. */
  const parts = () => PICK.filter(([id]) => (hasObs() || !id.startsWith('o')) && (hasPark() || !id.startsWith('pk')) && (!oneWay() || (id !== 'r2l' && id !== 'l2r') || id === laneId()))
    .map(([id, name, col]) => (oneWay() && id === laneId() ? [id, 'Lane', col] : [id, name, col]));
  const paintParts = () => {
    $('#parts').innerHTML = parts().map(([id, name, col]) => html`<button type="button" data-h="${id}" aria-pressed="${id === sel}"><i style="background:${col}"></i>${name}</button>`.__html).join('');
    $('#legend').innerHTML = html`<span><i style="background:#fff"></i>Box &amp; speed lines</span>
      ${oneWay() ? html`<span><i style="background:${LANE[draft.legalDirection]}"></i>Lane</span>`
        : html`<span><i style="background:${LANE.R2L}"></i>Right-to-left wheels</span><span><i style="background:${LANE.L2R}"></i>Left-to-right wheels</span>`}
      ${hasPark() ? html`<span><i style="background:${PARK}"></i>Parked cars' wheels</span>` : ''}`.__html;
    paintPark();
  };
  /**
   * The parking lane's card: how far beyond the moving cars VST makes it, from the last saved config (VST works it out
   * with its camera model, docs/companion-api.md parkingLaneBeyondFt), and the optional measured distance.
   */
  const paintPark = () => {
    $('#park-more').hidden = !hasPark();
    if (!hasPark()) return;
    const savedSame = JSON.stringify(cfg.parkingLane) === JSON.stringify(draft.parkingLane) && (cfg.parkingLaneFt || 0) === (draft.parkingLaneFt || 0);
    const b = cfg.parkingLaneBeyondFt, how = { picture: 'worked out from the picture and your camera', measured: 'as you measured', typical: "a typical guess; VST can't read it from this camera" }[cfg.parkingLaneHow];
    $('#park-est').textContent = !savedSame || b == null ? 'Save to see how far beyond the moving cars VST makes the parked cars.'
      : `Parked cars' wheels are about ${Math.abs(b).toFixed(1)} ft ${b >= 0 ? 'farther' : 'nearer'} than moving cars' (${how}).`;
    if (document.activeElement !== $('#park-ft')) $('#park-ft').value = draft.parkingLaneFt > 0 ? draft.parkingLaneFt : '';
  };
  /** The picked part's name, where it is, and the matching hint; arrows that can't move it are disabled. */
  const paintSel = () => {
    const name = parts().find(([id]) => id === sel)?.[1] || '';
    const pt = handlePoint(draft, sel, ed.k());
    $('#sel-name').textContent = name;
    $('#sel-pos').textContent = pt ? `${HANDLE_AXES[sel].includes('x') ? `x ${Math.round(pt[0])}` : ''}${HANDLE_AXES[sel] === 'xy' ? ' · ' : ''}${HANDLE_AXES[sel].includes('y') ? `y ${Math.round(pt[1])}` : ''} of ${W}×${H}` : '';
    pad.setAxes(HANDLE_AXES[sel]);
    $('#hint p').textContent = oneWay() && (sel === 'r2l' || sel === 'l2r') ? PARTS.lane : PARTS[sel];
  };
  const select = (h) => {
    sel = h;
    $$('#parts button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.h === h));
    $(`#parts [data-h="${h}"]`)?.scrollIntoView({ block: 'nearest', inline: 'nearest' });
    paintSel();
    ed.redraw();
  };
  /** Moves the picked handle to frame point pt through the same maths a drag uses. */
  const moveTo = (h, pt) => {
    Object.assign(draft, oneLane({ ...draft, ...applyHandle(draft, h, pt[0], pt[1], ed.k()) }));
    paintDirty();
    paintSel();
    ed.redraw();
    return handlePoint(draft, h, ed.k());
  };

  const ed = frameStage($('#ed'), {
    draw: (k) => { $('#svg').innerHTML = zonesSvg(draft, { k, editable: true, selected: sel }); },
    press: (pt) => {
      // The nearest handle within ~32 screen px wins, so overlapping handles (a lane dot near a speed line) are
      // still reachable: zoom in and they separate.
      const k = ed.k();
      let best = null, bd = 32 * k;
      for (const [id] of parts()) {
        const hp = handlePoint(draft, id, k);
        const d = hp && Math.hypot(hp[0] - pt[0], hp[1] - pt[1]);
        if (hp && d < bd) { best = id; bd = d; }
      }
      if (!best) return null;
      select(best);
      return { at: handlePoint(draft, best, k), move: (p) => moveTo(best, p) };
    },
  });
  const pad = nudgePad($('#pad'), (dx, dy) => {
    const pt = handlePoint(draft, sel, ed.k());
    if (pt) moveTo(sel, [pt[0] + dx, pt[1] + dy]);
  });
  $('#parts').onclick = (e) => { const h = e.target.closest('[data-h]')?.dataset.h; if (h) select(h); };
  paintParts();
  paintSel();

  $('#obs').onchange = (e) => {
    const g = geometry(draft);
    // A fresh obstruction starts as a narrow band in the middle of the speed zone, ready to drag.
    const mid = (g.sl + g.sr) / 2 - draft.AnalysisBoxLeft;
    draft.obstruction = e.target.checked ? [Math.round(mid - 30), Math.round(mid + 30)] : [0, 0];
    paintDirty();
    paintParts();
    select(e.target.checked ? 'o0' : sel.startsWith('o') ? 'tl' : sel);
  };

  // The parking lane starts parallel to a lane line, 50 px above the farther one (the far side, the usual view from a
  // window), or 50 px below the nearer one when there's no room above, so on a two-way street it never begins between
  // the lanes (as VST Studio does).  The user drags its ends onto a parked car's wheels.
  $('#park')?.addEventListener('change', (e) => {
    if (e.target.checked) {
      const g = geometry(draft);
      const line = (dir) => {
        const yl = dir === 'R2L' ? g.r2l : g.l2r, yr = dir === 'R2L' ? g.r2lR : g.l2rR;
        return (fx) => yl + (yr - yl) * (fx - g.box.x) / (g.box.w || 1);
      };
      const lines = (oneWay() ? [draft.legalDirection] : ['L2R', 'R2L']).map(line)
        .sort((a, b) => a(0) + a(W) - b(0) - b(W));
      let at = lines[0], off = -50;
      if (Math.min(at(0), at(W)) - 50 < 20) { at = lines[lines.length - 1]; off = 50; }
      draft.parkingLane = [at(0), at(W)].map((v) => Math.round(Math.max(1, Math.min(H - 1, v + off))));
    } else draft.parkingLane = [0, 0];
    paintDirty();
    paintParts();
    select(e.target.checked ? 'pkl' : sel.startsWith('pk') ? 'tl' : sel);
  });
  $('#park-ft').oninput = (e) => { const v = Number(e.target.value); draft.parkingLaneFt = v > 0 ? Math.min(40, v) : 0; paintDirty(); paintPark(); };

  // Rules of the road: the same draft.
  const paintRules = () => {
    $$('#legal button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === draft.legalDirection));
    $$('.stepper').forEach((s) => { s.querySelector('output').textContent = `${draft[s.dataset.k]} mph`; });
    $('#r2l').value = draft.R2LDirection;
    $('#l2r').value = draft.L2RDirection;
    $('#site').value = draft.siteName;
  };
  $('#legal').onclick = (e) => {
    const v = e.target.closest('[data-v]')?.dataset.v;
    if (!v) return;
    draft.legalDirection = v;
    // Going one-way: the one lane line is the legal direction's (the other follows it).
    Object.assign(draft, oneLane(draft));
    if ((sel === 'r2l' || sel === 'l2r') && oneWay()) sel = laneId();
    paintRules(); paintParts(); paintSel(); ed.redraw(); paintDirty();
  };
  $$('.stepper').forEach((s) => s.addEventListener('click', (e) => {
    const step = Number(e.target.closest('[data-d]')?.dataset.d || 0);
    if (!step) return;
    draft[s.dataset.k] = Math.max(5, Math.min(90, draft[s.dataset.k] + step));
    if (s.dataset.k === 'speedLimit' && draft.egregious <= draft.speedLimit) draft.egregious = draft.speedLimit + 10;
    paintRules(); paintDirty();
  }));
  $('#r2l').onchange = (e) => { draft.R2LDirection = e.target.value; paintDirty(); };
  $('#l2r').onchange = (e) => { draft.L2RDirection = e.target.value; paintDirty(); };
  $('#site').oninput = (e) => { draft.siteName = e.target.value; paintDirty(); };

  $('#reset').onclick = () => {
    Object.assign(draft, copy(cfg));
    $('#obs').checked = hasObs();
    if ($('#park')) $('#park').checked = hasPark();
    if (!parts().some(([id]) => id === sel)) sel = 'tl';
    paintParts(); paintRules(); select(sel); paintDirty();
  };
  /** Sends the page's changes; true when VST took them. */
  const save = async () => {
    const changed = MARK_KEYS.filter((k) => k in draft);
    const body2 = Object.fromEntries(changed.map((k) => [k, k === 'siteName' ? draft[k].trim() : draft[k]]));
    const r = await vst.putConfig(body2);
    if (!r.ok) { $('#problems').textContent = `${r.problems.join('. ')}.`; return false; }
    Object.assign(cfg, r.config);
    Object.assign(draft, copy(cfg));
    paintRules(); paintDirty(); paintPark();
    await vst.status().catch(() => {});
    // VST saves at once, but a run in progress keeps the zones it started with (docs/companion-api.md).
    toast(vst.lastStatus?.running ? 'Saved. VST uses the new zones from the next start.' : 'Saved. VST will use the new zones.');
    return true;
  };
  $('#save').onclick = save;
  paintRules();
  paintDirty();

  return () => { guard(null); ed.destroy(); };
}

// ---- 5. calibrate: a drive past, then a check drive ---------------------------------------------------

/**
 * The Known car picture (VST Studio's wheelbaseSketch): a side-on car with the two points to tap marked and the span
 * between them dimensioned, the wheel centres (wheels) or the bumpers.
 */
function carSketch(wheels) {
  const x0 = wheels ? 66 : 12, x1 = wheels ? 234 : 291, my = wheels ? 74 : 60;
  const mark = (x) => html`<g stroke="#fbbf24" fill="none"><circle cx="${x}" cy="${my}" r="7" stroke-width="2.5"/><path d="M${x - 11} ${my}h22M${x} ${my - 11}v22" stroke-width="2"/></g>`;
  return html`<svg class="car-sketch" viewBox="0 0 300 110" role="img" aria-label="${wheels ? 'Tap the middle of each wheel; the span is the wheelbase' : 'Tap each bumper; the span is the length'}" style="width:100%;max-width:360px;display:block;margin:0 auto">
    <rect x="12" y="40" width="279" height="34" rx="12" fill="#3a4660"/>
    <path d="M78 41 L108 14 H198 L234 41 Z" fill="#3a4660"/><path d="M93 39 L115 19 H190 L216 39 Z" fill="#55647f"/><path d="M153 19V39" stroke="#3a4660" stroke-width="3"/>
    ${[66, 234].map((x) => html`<circle cx="${x}" cy="74" r="18" fill="#0b0e14"/><circle cx="${x}" cy="74" r="10" fill="#8b95a8"/><circle cx="${x}" cy="74" r="3" fill="#0b0e14"/>`)}
    ${mark(x0)}${mark(x1)}
    <path d="M${x0} 100H${x1}M${x0} 94v12M${x1} 94v12" stroke="#8b95a8" stroke-width="1.5"/>
    <rect x="${(x0 + x1) / 2 - 34}" y="88" width="68" height="14" fill="var(--surface-2, #171d2a)"/>
    <text x="${(x0 + x1) / 2}" y="99" text-anchor="middle" fill="currentColor" font-size="11">${wheels ? 'wheelbase' : 'length'}</text>
  </svg>`;
}

const DIR_WORDS = { L2R: 'left to right', R2L: 'right to left' };

/** A direction's calibration standing (GET /api/status `calibration`) as a pill. */
function calPill(c) {
  if (!c) return '';
  if (c.state === 'verified') return html`<span class="pill ok">${icon('check')}Verified ±${Math.abs(c.error).toFixed(1)}</span>`;
  if (c.state === 'failed') return html`<span class="pill bad">${icon('warn')}Off by ${Math.abs(c.error).toFixed(1)}</span>`;
  if (c.state === 'none') return html`<span class="pill warn">Not calibrated</span>`;
  return html`<span class="pill">Not checked</span>`;
}

/**
 * The drive step runs in two phases on one screen:
 *   calibrate - tap your pass, confirm, and VST sets the calibration for that direction (POST /api/calibration/apply);
 *   check     - straight after, drive once more (ideally at another speed) and tap that pass. POST /api/calibration/check
 *               says what the new calibration makes of it, and the result card shows Verified or Off by X mph.
 * The pass list, the speed stepper and the hand-marking fallback serve both; `phase` decides what a tap does.
 */
function drive({ vst, body, cfg, st, n }) {
  const opened = Date.now();
  const recent = [];
  let speed = cfg.CalibrationSpeed || st.speedLimit;
  let phase = 'calibrate';
  let current = cfg;   // the config as the latest apply left it
  let appliedId = 0;   // the pass the calibration came from: checking it against itself proves nothing
  const preselect = Number(sessionStorage.getItem('vst.calPass') || 0);
  sessionStorage.removeItem('vst.calPass');

  body.innerHTML = html`
    <h1>Calibrate</h1>
    <p class="lede">VST learns how fast a vehicle is going by comparing it with one whose speed you know: yours.</p>
    <div class="card">
      <div class="check"><b class="num" style="color:var(--accent)">1</b><p><b>Pick a calm moment</b> and set cruise control (or hold steady) at the speed below.</p></div>
      <div class="check"><b class="num" style="color:var(--accent)">2</b><p><b>Drive past the camera</b> at that speed the legal way${st.legalDirection === 'both' ? ', then once the other way' : ''}. Keep this screen open; your pass appears below within seconds.</p></div>
      <div class="check"><b class="num" style="color:var(--accent)">3</b><p><b>Tap your pass</b> and confirm.</p></div>
      <div class="check"><b class="num" style="color:var(--accent)">4</b><p><b>Drive once more to check.</b> Ideally at a different speed; VST says how close it gets.</p></div>
    </div>
    <details id="cones" class="card" style="margin-top:12px"><summary><b>No car handy? Calibrate with cones or a pole</b><br><small class="muted">Stand on the street with this phone, see yourself on the camera, and set it up there.</small></summary>
      <div style="margin-top:10px">
        <div class="check"><b class="num" style="color:var(--accent)">a</b><p><b>Chalk two spots</b> 10 ft or more apart along the lane, where the cars' near wheels run. Stand a cone on each, or hold a pole (or a broom) upright on one spot, then the other.</p></div>
        <div class="check"><b class="num" style="color:var(--accent)">b</b><p><b>Tap each spot</b> on the live picture below. Any point on an upright pole will do, so one person can tap spot 1, walk the pole to spot 2 and tap again. Pinch or tap + to zoom in; drag a placed spot or use the arrows to fine-tune it.</p></div>
        <div class="check"><b class="num" style="color:var(--accent)">c</b><p><b>Enter the taped distance</b> and apply. Then drive past once to check it when you can.</p></div>
        <div class="editor" id="ced" style="margin-top:10px"><span id="clive"></span><svg viewBox="0 0 ${W} ${H}" id="csvg"></svg></div>
        <div class="fine">
          <div class="seg grow" id="cwhich"><button data-v="0" aria-pressed="true">Spot 1</button><button data-v="1">Spot 2</button></div>
          <div id="cpad"></div>
        </div>
        <label class="field" style="margin-top:10px"><span>Spots apart (ft)</span><input class="input" id="cft" type="number" inputmode="decimal" min="1" step="0.5" value="10"></label>
        <div class="seg" id="cdir" style="margin-top:10px"><button data-v="both" aria-pressed="true">Both ways</button><button data-v="R2L">← only</button><button data-v="L2R">→ only</button></div>
        <p class="small muted" id="cprev" style="margin-top:10px"></p>
        <div class="btn-row"><button class="btn" id="cclear">Clear spots</button><button class="btn primary" id="capply" disabled>Apply</button></div>
      </div>
    </details>
    <details id="kcar" class="card" style="margin-top:12px"><summary><b>Know a car's model? Calibrate from its wheels</b><br><small class="muted">A parked car works: no driving, no tape.</small></summary>
      <div style="margin-top:10px">
        ${carSketch(true)}
        <p class="small" id="khow-text" style="margin:6px 0 10px"></p>
        <div class="seg" id="khow"><button data-v="wheels" aria-pressed="true">Wheel centres</button><button data-v="bumpers">Bumpers</button></div>
        <div class="check" style="margin-top:10px"><b class="num" style="color:var(--accent)">a</b><p><b>Get a still</b> with the car side-on and as big as it gets, near the middle of the picture.</p></div>
        <div class="check"><b class="num" style="color:var(--accent)">b</b><p id="kstep-b"></p></div>
        <div class="check"><b class="num" style="color:var(--accent)">c</b><p><b>Pick the model,</b> check where VST says the car is, and apply. Then drive past once to check it.</p></div>
        <div class="editor" id="ked" style="margin-top:10px"><span id="kimg"></span><svg viewBox="0 0 ${W} ${H}" id="ksvg"></svg></div>
        <div class="seg" id="kwhich" style="margin-top:10px"><button data-v="0" aria-pressed="true"></button><button data-v="1"></button></div>
        <div class="fine">
          <div class="grow"><small>Tap the picture to place the selected mark, then nudge it with the arrows.</small></div>
          <div id="kpad"></div>
        </div>
        <button class="btn small" id="kshot" style="margin-top:8px">${icon('camera')}New picture</button>
        <label class="field" style="margin-top:10px"><span>Model</span><select class="input" id="kmodel"><option value="-1">Other (type it)</option></select></label>
        <label class="field" style="margin-top:8px"><span id="kft-label">Wheelbase (ft)</span><input class="input" id="kft" type="number" inputmode="decimal" min="1" step="0.01" value="9.25"></label>
        <div id="kwhere" style="margin-top:10px"></div>
        <div class="seg" id="kpark" style="margin-top:8px"><button data-v="auto" aria-pressed="true">Auto</button><button data-v="lane">In the travel lane</button><button data-v="parked">Parked</button></div>
        <label class="field" id="kbeyond-row" style="margin-top:8px" hidden><span>Parked farther than moving cars by (ft)</span><input class="input" id="kbeyond" type="number" inputmode="decimal" step="0.5" value="7">
          <span class="small muted" style="font-weight:400">Across the street, from a passing car's wheels to the parked car's. Nearer (between you and traffic): negative.</span></label>
        <div class="seg" id="kdir" style="margin-top:10px"><button data-v="both" aria-pressed="true">Both ways</button><button data-v="R2L">← only</button><button data-v="L2R">→ only</button></div>
        <p class="small muted" id="kprev" style="margin-top:10px"></p>
        <div class="btn-row"><button class="btn" id="kclear">Clear marks</button><button class="btn primary" id="kapply" disabled>Apply</button></div>
      </div>
    </details>
    <div id="phase"></div>
    <h3>Speed you will drive</h3>
    <div class="stepper" style="max-width:240px"><button data-d="-1" aria-label="Slower">−</button><output id="spd" aria-live="polite"></output><button data-d="1" aria-label="Faster">+</button></div>
    <div id="run"></div>
    <div class="section-head"><h3>Passes since you opened this</h3><span class="pill small" id="listening"></span></div>
    <div id="passes"></div>
    <div id="result"></div>
    <h3>Current calibration</h3>
    <div class="list" id="cal"></div>
    <details style="margin-top:8px"><summary class="small muted">No pass showing up? Count it by hand</summary>
      <div class="card" style="margin-top:10px">
        <p class="small">Watching the Live view, tap <b>Start</b> as your front bumper crosses the first white line and <b>End</b> at the second. Phone video lags a little, so this is less precise than using a logged pass.</p>
        <div class="seg" id="mdir" style="margin-bottom:10px"><button data-v="R2L" aria-pressed="true">← Right to left</button><button data-v="L2R">Left to right →</button></div>
        <div class="btn-row"><button class="btn" data-edge="start">Start</button><button class="btn" data-edge="end">End</button><button class="btn primary" id="mapply">Apply</button></div>
      </div>
    </details>
    ${nav(n)}`.__html;

  const paintSpeed = () => { $('#spd').textContent = `${speed} mph`; };
  $('.stepper').onclick = (e) => { const d = Number(e.target.closest('[data-d]')?.dataset.d || 0); if (d) { speed = Math.max(5, Math.min(60, speed + d)); paintSpeed(); } };

  /** The banner above the speed stepper says which phase this is, and the hand-marking button follows it. */
  const paintPhase = () => {
    $('#phase').innerHTML = phase === 'check'
      ? html`<div class="callout" style="margin-top:12px">${icon('ruler')}<p><b>Now drive once more to check it.</b> Pick a different speed from the first pass if you can, set it below, and tap the new pass. Nothing changes; VST just tells you how close it gets.
          <br><button class="btn ghost small" id="recal" style="margin-top:8px">Calibrate again instead</button></p></div>`.__html
      : '';
    $('#recal')?.addEventListener('click', () => { phase = 'calibrate'; $('#result').innerHTML = ''; paintPhase(); paintPasses(); });
    $('#mapply').textContent = phase === 'check' ? 'Check' : 'Apply';
  };

  const paintCal = (c, cal) => {
    const row = (dir, label) => html`
      <div class="item"><div class="label"><b>${label}</b><small>Frames to cross the zone at ${c.CalibrationSpeed} mph</small></div>
        <div class="value">${c[`CalibrationFrames${dir}`]} frames<br>${calPill(cal?.[dir])}</div></div>`;
    $('#cal').innerHTML = html`
      ${row('R2L', '← Right to left')}
      ${row('L2R', 'Left to right →')}
      <div class="item"><div class="label"><b>Measured at</b></div><div class="value">${c.CalibrationFPS} fps</div></div>`.__html;
  };
  /** Re-reads the calibration standing after anything that changes it (an apply or a check). */
  const refreshCal = async (c) => { const s = await vst.status().catch(() => null); paintCal(c, s?.calibration); };

  const paintPasses = () => {
    const action = (v) => phase === 'check' ? (v.id === appliedId ? 'Calibrated from' : 'Check with this') : 'That was me';
    $('#passes').innerHTML = recent.length
      ? recent.map((v) => html`<button class="pass" style="width:100%;text-align:left;cursor:pointer" data-id="${v.id}" ${phase === 'check' && v.id === appliedId ? 'disabled' : ''}>
          ${sign(v.speed, band(v))}
          <div class="meta"><b>${v.dir === 'L2R' ? 'Left to right' : 'Right to left'} ${arrow(v.dir)}</b><small>${clock(v.time)} · ${ago(v.time)} · ${v.frames} frames</small></div>
          <span class="btn ghost small">${action(v)}</span></button>`.__html).join('')
      : html`<div class="empty-state" style="padding:28px 16px">${icon('car')}<p>Waiting for a vehicle to cross the speed zone…</p></div>`.__html;
  };

  /** After an apply: show the new values and move on to the check drive. */
  const calibrated = (r, id) => {
    appliedId = id || 0;
    current = r.config;
    phase = 'check';
    $('#result').innerHTML = '';
    refreshCal(r.config);
    paintPhase();
    paintPasses();
  };

  /** The check result: what you drove, what VST says, the error, and the verdict in words. */
  const showResult = (r) => {
    const off = Math.abs(r.error).toFixed(1);
    $('#result').innerHTML = html`
      <h3>Check, ${DIR_WORDS[r.direction]}</h3>
      <div class="kpis three">
        <div class="kpi"><b>${r.drivenMph}</b><span>You drove (mph)</span></div>
        <div class="kpi"><b>${r.measuredMph.toFixed(1)}</b><span>VST says (mph)</span></div>
        <div class="kpi ${r.verified ? 'ok' : 'bad'}"><b>${r.error > 0 ? '+' : ''}${r.error.toFixed(1)}</b><span>Error (mph)</span></div>
      </div>
      ${r.verified
        ? html`<div class="callout ok" style="margin-top:12px">${icon('check')}<p><b>Verified.</b> VST is within ${off} mph of your speedometer on a second, independent pass. Both drives are on record in VST.</p></div>`
        : html`<div class="callout bad" style="margin-top:12px">${icon('warn')}<p><b>Off by ${off} mph.</b> More than ±2 mph, so this isn't verified yet. Usual causes: your speedometer reads high (a GPS speed app is a better reference), the camera or zones moved, or the two passes were in different lanes.
            <br><button class="btn small" id="again" style="margin-top:8px">Calibrate again</button></p></div>`}`.__html;
    $('#again')?.addEventListener('click', () => { phase = 'calibrate'; $('#result').innerHTML = ''; paintPhase(); paintPasses(); });
  };

  const check = async (req) => {
    const r = await vst.calibrationCheck(req);
    if (!r.ok) { toast(r.problems.join('. ')); return; }
    showResult(r);
    refreshCal(current);
    $('#result').scrollIntoView({ behavior: 'smooth', block: 'nearest' });
  };

  /** Confirms and applies a pass as the calibration for its direction. */
  const choose = async (v) => {
    const okd = await sheet(html`
      <h2>Calibrate from this pass?</h2>
      <p>You drove <b>${DIR_WORDS[v.dir]}</b> at <b>${speed} mph</b> and took <b>${v.frames} frames</b> to cross the zone. VST will use this for every ${v.dir === 'L2R' ? 'left-to-right' : 'right-to-left'} vehicle.</p>
      <div class="btn-row"><button class="btn" data-v="0">Cancel</button><button class="btn primary" data-v="1">Calibrate</button></div>`,
    (el, close) => $$('[data-v]', el).forEach((b) => b.onclick = () => close(b.dataset.v === '1')));
    if (!okd) return;
    const r = await vst.calibrationApply({ dir: v.dir, speed, vehicleId: v.id });
    if (!r.ok) { toast(r.problems.join('. ')); return; }
    toast(html`${icon('check')}<span>Calibrated ${DIR_WORDS[v.dir]}: ${v.frames} frames at ${speed} mph</span>`);
    calibrated(r, v.id);
  };
  $('#passes').onclick = (e) => {
    const id = Number(e.target.closest('[data-id]')?.dataset.id);
    const v = recent.find((x) => x.id === id);
    if (!v) return;
    // A check changes nothing, so it needs no confirmation.
    if (phase === 'check') { if (v.id !== appliedId) check({ dir: v.dir, speed, vehicleId: v.id }); }
    else choose(v);
  };

  /**
   * Two spots a measured distance apart (POST /api/calibration/distance): two cones, or one pole moved from the first
   * spot to the second, for setting up from the street with no car. Only x counts, so any point on an upright pole works.
   * The live picture plays only while the section is open, so it doesn't hold one of Studio's few stream slots.
   * Taps land in frame coordinates (1280×720, like the speed lines); each tap places the selected cone and moves the
   * selection to the other one. The picture is a touch.js stage: pinch or + zooms, a placed spot can be dragged (it moves
   * by the finger's travel, so the finger doesn't hide it) and the arrows move the selected spot a pixel at a time.
   * No loupe here: it would open a second copy of the live stream. The preview repeats the server's arithmetic (Calibration.h framesForDistance) so the
   * zone length and frames are visible before applying.
   */
  const cones = [null, null];
  let cone = 0, cdir = 'both';
  const csvg = $('#csvg');
  let cst = null;   // the zoomable stage, created below once paintCones exists
  const paintCones = () => {
    const g = geometry(current), k = cst ? cst.k() : W / ($('#ced').clientWidth || 360);
    const marks = cones.map((c, i) => c && html`
      <line x1="${c[0]}" y1="0" x2="${c[0]}" y2="${H}" stroke="#4ade80" stroke-width="${2 * k}" stroke-dasharray="${8 * k} ${6 * k}"/>
      <path d="M${c[0]} ${c[1] - 26 * k} L${c[0] - 12 * k} ${c[1]} L${c[0] + 12 * k} ${c[1]} Z" fill="#fb923c" stroke="#000" stroke-width="${1.5 * k}"/>
      <text x="${c[0] + 14 * k}" y="${c[1] - 8 * k}" fill="#fff" stroke="#000" stroke-width="${3 * k}" paint-order="stroke" font-size="${16 * k}" font-weight="700">${i + 1}</text>`.__html).join('');
    csvg.innerHTML = zonesSvg(current, { k }) + marks;
    const feet = Number($('#cft').value), markPx = cones[0] && cones[1] ? Math.abs(cones[1][0] - cones[0][0]) : 0;
    const zoneFt = markPx && feet > 0 ? (g.sr - g.sl) / markPx * feet : 0;
    const frames = zoneFt / (current.CalibrationSpeed * 5280 / 3600) * current.CalibrationFPS;
    $('#cprev').textContent = !(cones[0] && cones[1]) ? `Tap spot ${cone + 1} on the picture.`
      : markPx < 40 ? 'The two spots are too close together on the picture; space them further apart.'
      : !(feet > 0) ? 'Enter the distance between the spots.'
      : `Zone between the speed lines: ${zoneFt.toFixed(1)} ft, ${frames.toFixed(1)} frames at ${current.CalibrationSpeed} mph (${current.CalibrationFPS} fps).`;
    $('#capply').disabled = !(cones[0] && cones[1] && markPx >= 40 && feet > 0);
    $$('#cwhich button').forEach((b) => b.setAttribute('aria-pressed', Number(b.dataset.v) === cone));
  };
  const live = () => {
    const src = vst.liveSource();
    $('#clive').innerHTML = src.kind === 'video'
      ? html`<video src="${src.src}" autoplay muted loop playsinline style="position:absolute;inset:0;width:100%;height:100%;object-fit:fill"></video>`.__html
      : html`<img src="${src.src}" alt="Live camera view">`.__html;
  };
  const stopLive = () => { const m = $('#clive img, #clive video'); if (m) { m.removeAttribute('src'); m.remove(); } };
  $('#cones').ontoggle = () => { if ($('#cones').open) { live(); paintCones(); } else stopLive(); };
  const placeCone = (i, [fx, fy]) => { cones[i] = [Math.round(Math.max(0, Math.min(W, fx))), Math.round(Math.max(0, Math.min(H, fy)))]; };
  cst = frameStage($('#ced'), {
    loupe: false,
    draw: () => paintCones(),
    // A finger within ~32 screen px of a placed spot drags it; anywhere else a tap places the selected spot.
    press: (pt) => {
      const k = cst.k();
      const i = [0, 1].map((j) => [j, cones[j] && Math.hypot(cones[j][0] - pt[0], cones[j][1] - pt[1])])
        .filter(([, d]) => d != null && d < 32 * k).sort((a, b) => a[1] - b[1])[0]?.[0];
      if (i == null) return null;
      cone = i;
      return { at: [...cones[i]], move: (p) => { placeCone(i, p); paintCones(); return cones[i]; } };
    },
    tap: (pt) => {
      placeCone(cone, pt);
      if (!cones[1 - cone]) cone = 1 - cone;
      paintCones();
    },
  });
  nudgePad($('#cpad'), (dx) => {
    if (!cones[cone]) return;
    placeCone(cone, [cones[cone][0] + dx, cones[cone][1]]);
    paintCones();
  }, 'x');
  $('#cwhich').onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v != null) { cone = Number(v); paintCones(); } };
  $('#cdir').onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v) { cdir = v; $$('#cdir button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === v)); } };
  $('#cft').oninput = paintCones;
  $('#cclear').onclick = () => { cones[0] = cones[1] = null; cone = 0; paintCones(); };
  $('#capply').onclick = async () => {
    const r = await vst.calibrationDistance({ cones, feet: Number($('#cft').value), dir: cdir });
    if (!r.ok) { toast(r.problems.join('. ')); return; }
    toast(html`${icon('check')}<span>Calibrated from the measured spots: ${r.frames} frames for a ${r.zoneFeet} ft zone. Check it with one drive past.</span>`);
    calibrated(r, 0);
  };

  /**
   * Known car (POST /api/calibration/car): two marks on a car of a known model, its wheel centres (the wheelbase) or its
   * bumpers (the length), on a still from the camera.  VST answers, for a preview or an apply, whether the car is parked
   * (from the parking lane drawn in step 4, or as chosen here) and how much it scales a parked car to the travel lane; the
   * page shows that before anything is applied.  The marks become VST Studio's own Known car marks too, so the computer
   * shows the same car.  Marks are placed like the cones: tap to place the selected one, drag a placed one, nudge it.
   */
  const kmarks = [null, null];
  let kmark = 0, khow = 'wheels', kwhere = 'auto', kdir = 'both', kcars = [], kprev = null, kseq = 0;
  let kst = null;
  const kfeet = () => Number($('#kft').value);
  const paintKHow = () => {
    const w = khow === 'wheels';
    $('#kcar .car-sketch').outerHTML = carSketch(w).__html;
    $('#khow-text').textContent = w
      ? 'The middle of each wheel is a sharp point at the lane line\'s height, so a car at a slight angle, rounded corners or a bike rack don\'t matter. The easiest and most accurate choice.'
      : 'Only when the wheels are hidden. Bumpers are rounded, and at an angle the camera sees some of the front or back, which reads as extra length.';
    $('#kstep-b').innerHTML = w ? html`<b>Tap the middle of each wheel</b> on the side facing you. Zoom in to get them exact.`.__html
      : html`<b>Tap each bumper</b> where it ends on the side facing the camera. Zoom in to get them exact.`.__html;
    $$('#kwhich button').forEach((b, i) => { b.textContent = `${i ? 'Right' : 'Left'} ${w ? 'wheel' : 'bumper'}`; });
    $('#kft-label').textContent = w ? 'Wheelbase (ft)' : 'Length (ft)';
    $$('#khow button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === khow));
  };
  const paintK = () => {
    const k = kst ? kst.k() : W / ($('#ked').clientWidth || 360);
    const col = '#fbbf24';
    const marks = kmarks.map((m, i) => m && html`
      <circle cx="${m[0]}" cy="${m[1]}" r="${9 * k}" fill="none" stroke="${col}" stroke-width="${2.5 * k}"/>
      <line x1="${m[0] - 15 * k}" y1="${m[1]}" x2="${m[0] + 15 * k}" y2="${m[1]}" stroke="${col}" stroke-width="${2 * k}"/>
      <line x1="${m[0]}" y1="${m[1] - 15 * k}" x2="${m[0]}" y2="${m[1] + 15 * k}" stroke="${col}" stroke-width="${2 * k}"/>
      <text x="${m[0] + 12 * k}" y="${m[1] - 12 * k}" fill="#fff" stroke="#000" stroke-width="${3 * k}" paint-order="stroke" font-size="${14 * k}" font-weight="700">${i ? 'R' : 'L'}</text>`.__html).join('');
    let tag = '';
    if (kmarks[0] && kmarks[1] && kprev) {
      const parked = kprev.where === 'parked', off = kprev.offLines;
      const text = off ? 'NOT ON A LINE' : parked ? `PARKED · ×${(kprev.scale ?? 1).toFixed(2)}` : 'IN THE TRAVEL LANE';
      const fill = off ? '#fbbf24' : parked ? PARK : LANE[current.legalDirection === 'L2R' ? 'L2R' : 'R2L'];
      const x = Math.max(99 * k, Math.min(W - 99 * k, (kmarks[0][0] + kmarks[1][0]) / 2)), y = Math.max(30 * k, Math.min(kmarks[0][1], kmarks[1][1]) - 40 * k);
      tag = html`<g><rect x="${x - 95 * k}" y="${y - 16 * k}" width="${190 * k}" height="${24 * k}" rx="${6 * k}" fill="${fill}"/>
        <text x="${x}" y="${y + 1 * k}" text-anchor="middle" fill="#0b0d10" font-size="${12 * k}" font-weight="800">${text}</text></g>`.__html;
    }
    $('#ksvg').innerHTML = zonesSvg(current, { k }) + marks + tag;
    $$('#kwhich button').forEach((b) => b.setAttribute('aria-pressed', Number(b.dataset.v) === kmark));
  };
  /** What VST says about the car (a preview: nothing changes), shown under the picker and on the picture. */
  const paintKWhere = () => {
    const p = kprev, park = !!current.parkingLane && current.parkingLane[0] > 0;
    $('#kbeyond-row').hidden = !(kwhere === 'parked' && !park);
    $$('#kpark button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === kwhere));
    if (!(kmarks[0] && kmarks[1])) {
      $('#kwhere').innerHTML = park ? html`<p class="small muted" style="margin:0">Mark the car and VST says whether it's parked, from the parking lane in step 4.</p>`.__html
        : html`<p class="small muted" style="margin:0">${icon('info')} Tip: draw the parking lane in <a href="#/setup/4">step 4</a> and VST tells parked cars from passing ones by itself.</p>`.__html;
      return;
    }
    if (!p) { $('#kwhere').innerHTML = ''; return; }
    const how = { picture: 'from your picture and camera', measured: 'as you measured', typical: 'a typical guess', typed: 'as typed below' }[p.beyondHow] || '';
    $('#kwhere').innerHTML = p.offLines
      ? html`<div class="callout warn">${icon('warn')}<p>The marks aren't on the lane line or the parking lane. Tap the middle of each wheel, or move the lines in step 4.</p></div>`.__html
      : html`<div class="row" style="gap:8px;flex-wrap:wrap"><span class="pill ${p.where === 'parked' ? '' : 'ok'}" style="${p.where === 'parked' ? `background:${PARK};color:#0b0d10` : ''}">${p.where === 'parked' ? 'Parked' : 'In the travel lane'}</span>
          ${p.detected && kwhere !== 'auto' && p.detected !== p.where ? html`<span class="small muted">Its wheels look ${p.detected === 'parked' ? 'parked' : 'in the travel lane'}.</span>` : ''}</div>
        ${p.where === 'parked' && p.beyondFt != null ? html`<p class="small muted" style="margin:6px 0 0">About ${Math.abs(p.beyondFt).toFixed(1)} ft ${p.beyondFt >= 0 ? 'farther' : 'nearer'} than moving cars (${how}), so VST scales it ×${p.scale.toFixed(2)} to the travel lane.</p>` : ''}`.__html;
  };
  const kbody = (preview) => ({ marks: kmarks, feet: kfeet(), dir: kdir, where: kwhere, how: khow, preview,
    ...(kwhere === 'parked' ? { beyondFt: Number($('#kbeyond').value) || 0 } : {}) });
  /** Asks VST for a preview after any change; a slower, older answer never overwrites a newer one. */
  const previewK = async () => {
    const ready = kmarks[0] && kmarks[1];
    $('#kapply').disabled = true;
    if (!ready) { kprev = null; $('#kprev').textContent = `Tap the ${kmark ? 'right' : 'left'} ${khow === 'wheels' ? 'wheel' : 'bumper'} on the picture.`; paintKWhere(); paintK(); return; }
    const seq = ++kseq;
    const r = await vst.calibrationCar(kbody(true)).catch(() => null);
    if (seq !== kseq) return;
    kprev = r && r.where ? r : null;
    $('#kprev').textContent = !r ? "VST didn't answer." : !r.ok ? `${r.problems.join('. ')}.`
      : `Zone between the speed lines: ${r.zoneFeet} ft, ${r.frames} frames at ${current.CalibrationSpeed} mph (${current.CalibrationFPS} fps).`;
    $('#kapply').disabled = !(r && r.ok);
    paintKWhere();
    paintK();
  };
  const kshot = () => {
    const m = $('#kimg img');
    if (m) m.remove();
    $('#kimg').innerHTML = html`<img src="${vst.frameUrl()}" alt="A still from the camera">`.__html;
  };
  $('#kcar').ontoggle = async () => {
    if (!$('#kcar').open) return;
    if (!$('#kimg img')) kshot();
    if (!kcars.length) {
      const r = await vst.calibrationCars().catch(() => null);
      kcars = r?.cars || [];
      $('#kmodel').innerHTML = html`${kcars.map((c, i) => html`<option value="${i}">${c.name}</option>`)}<option value="-1">Other (type it)</option>`.__html;
      $('#kmodel').value = '-1';
    }
    paintKHow(); previewK();
  };
  const placeK = (i, [fx, fy]) => { kmarks[i] = [Math.round(Math.max(0, Math.min(W, fx))), Math.round(Math.max(0, Math.min(H, fy)))]; };
  kst = frameStage($('#ked'), {
    draw: () => paintK(),
    press: (pt) => {
      const k = kst.k();
      const i = [0, 1].map((j) => [j, kmarks[j] && Math.hypot(kmarks[j][0] - pt[0], kmarks[j][1] - pt[1])])
        .filter(([, d]) => d != null && d < 32 * k).sort((a, b) => a[1] - b[1])[0]?.[0];
      if (i == null) return null;
      kmark = i;
      return { at: [...kmarks[i]], move: (p) => { placeK(i, p); paintK(); previewK(); return kmarks[i]; } };
    },
    tap: (pt) => { placeK(kmark, pt); if (!kmarks[1 - kmark]) kmark = 1 - kmark; previewK(); },
  });
  nudgePad($('#kpad'), (dx, dy) => { if (!kmarks[kmark]) return; placeK(kmark, [kmarks[kmark][0] + dx, kmarks[kmark][1] + dy]); previewK(); });
  const segK = (id, set) => { $(id).onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v != null) { set(v); $$(`${id} button`).forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === v)); previewK(); } }; };
  segK('#kwhich', (v) => { kmark = Number(v); });
  segK('#khow', (v) => {
    if (v === khow) return;
    khow = v;
    kmarks[0] = kmarks[1] = null;
    kmark = 0;
    const c = kcars[Number($('#kmodel').value)];
    $('#kft').value = c ? ((v === 'wheels' ? c.wheelbaseIn : c.lengthIn) / 12).toFixed(2) : v === 'wheels' ? '9.25' : '15.50';
    paintKHow();
  });
  segK('#kpark', (v) => { kwhere = v; });
  segK('#kdir', (v) => { kdir = v; });
  $('#kmodel').onchange = (e) => {
    const c = kcars[Number(e.target.value)];
    if (c) $('#kft').value = ((khow === 'wheels' ? c.wheelbaseIn : c.lengthIn) / 12).toFixed(2);
    previewK();
  };
  $('#kft').oninput = () => { $('#kmodel').value = '-1'; previewK(); };
  $('#kbeyond').oninput = previewK;
  $('#kshot').onclick = kshot;
  $('#kclear').onclick = () => { kmarks[0] = kmarks[1] = null; kmark = 0; previewK(); };
  $('#kapply').onclick = async () => {
    const r = await vst.calibrationCar(kbody(false));
    if (!r.ok) { toast(r.problems.join('. ')); return; }
    toast(html`${icon('check')}<span>Calibrated from the ${khow === 'wheels' ? "car's wheelbase" : "car's length"}${r.where === 'parked' ? ' (parked, scaled to the travel lane)' : ''}: ${r.frames} frames for a ${r.zoneFeet} ft zone. Check it with one drive past.</span>`);
    calibrated(r, 0);
  };

  // Manual marking fallback.
  let mdir = 'R2L';
  $('#mdir').onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v) { mdir = v; $$('#mdir button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === v)); } };
  $$('[data-edge]').forEach((b) => b.onclick = async () => { await vst.calibrationMark(mdir, b.dataset.edge); toast(`Marked ${b.dataset.edge}`); });
  $('#mapply').onclick = async () => {
    if (phase === 'check') return check({ dir: mdir, speed });
    const r = await vst.calibrationApply({ dir: mdir, speed });
    if (!r.ok) toast(r.problems.join('. ')); else { toast('Calibrated by hand'); calibrated(r, 0); }
  };

  const onVehicle = (e) => { recent.unshift(e.detail); paintPasses(); $('#passes .pass')?.classList.add('fresh'); };
  vst.addEventListener('vehicle', onVehicle);

  // Show passes from the last few minutes too, in case the drive happened before opening this screen,
  // and offer straight away a pass picked from its detail page.
  (async () => {
    const since = new Date(opened - 10 * 60e3).toISOString();
    const earlier = await vst.vehicles({ from: since, limit: 20 });
    recent.push(...earlier.filter((v) => !recent.some((r) => r.id === v.id)));
    if (preselect) {
      const v = recent.find((x) => x.id === preselect) || await vst.vehicle(preselect).catch(() => null);
      if (v) { if (!recent.includes(v)) recent.unshift(v); paintPasses(); choose(v); return; }
    }
    paintPasses();
  })();

  /**
   * Passes only show up while VST is tracking, so a stopped or paused VST says so here, with the button that fixes it,
   * instead of "Listening" over a list that will never fill.
   */
  const paintRun = (s2) => {
    const on = s2?.running && !s2.paused;
    $('#listening').className = `pill small ${on ? 'rec' : 'warn'}`;
    $('#listening').innerHTML = on ? html`<i style="width:7px;height:7px;border-radius:50%;background:var(--ok)"></i>Listening`.__html : (s2?.running ? 'Paused' : 'Not tracking');
    $('#run').innerHTML = on ? '' : html`<div class="callout warn" style="margin-top:12px">${icon('warn')}<div class="grow">
        <p><b>VST isn't tracking${s2?.running ? ' (paused)' : ''}</b>, so your pass won't show up. Start it before you drive past.</p>
        <button class="btn primary small" id="start-run" style="margin-top:8px">${icon('play')}${s2?.running ? 'Resume tracking' : 'Start tracking'}</button></div></div>`.__html;
    $('#start-run')?.addEventListener('click', async () => {
      const r = await vst.run(s2?.running ? 'resume' : 'start').catch(() => null);
      const now = r?.status || await vst.status().catch(() => null);
      paintRun(now);
      if (!(now?.running && !now.paused)) toast("VST didn't start. Check that a camera or video is picked in VST Studio's step 3.");
    });
  };
  const onStatus = () => paintRun(vst.lastStatus);
  vst.addEventListener('status', onStatus);

  paintSpeed(); paintCal(cfg, st.calibration); paintPhase(); paintPasses(); paintRun(st);
  return () => { vst.removeEventListener('vehicle', onVehicle); vst.removeEventListener('status', onStatus); stopLive(); cst.destroy(); kst.destroy(); };
}
