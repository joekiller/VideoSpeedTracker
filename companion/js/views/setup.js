/**
 * Setup: a four-step guided calibration a first-time user can follow from the phone while standing
 * at the camera (or out on the street for step 4).
 *
 *   1. Camera & spot   - what a good view looks like, checked against the live frame;
 *   2. Mark the street - drag the analysis box, speed lines, lane lines and obstruction on the frame;
 *   3. Rules           - which way is legal, compass headings, limit, "way too fast", site name;
 *   4. Calibration drive - drive through at a known speed, tap your pass, then drive once more to check it;
 *      or, with no car handy, tap two spots a measured distance apart (cones, or one pole moved between them).
 *
 * Steps 2 and 3 edit a draft and save it with PUT /api/config; VST validates and the problems it
 * reports are shown in plain words. Step 4 uses POST /api/calibration/apply with the chosen pass,
 * then POST /api/calibration/check with a second one.
 */
import { html, $, $$, icon, sign, band, arrow, clock, ago, toast, sheet, confirmSheet } from '../ui.js';
import { zonesSvg, applyHandle, geometry, W, H, LANE } from '../overlay.js';

const STEPS = ['Camera & spot', 'Mark the street', 'Rules of the road', 'Calibration drive'];
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

export async function render(ctx, step = '1') {
  const n = Math.min(4, Math.max(1, Number(step)));
  const cfg = await ctx.vst.config();
  const st = await ctx.vst.status();
  ctx.view.innerHTML = html`
    <div class="step-kicker">Step ${n} of 4</div>
    <nav class="steps" aria-label="Setup steps">${STEPS.map((s, i) => html`<a href="#/setup/${i + 1}" title="${s}" aria-label="Step ${i + 1}: ${s}" ${i + 1 === n ? 'aria-current="step"' : ''} class="${i + 1 < n ? 'done' : i + 1 === n ? 'current' : ''}"></a>`)}</nav>
    <div id="step"></div>`.__html;
  const body = $('#step');
  return [spot, mark, rules, drive][n - 1]({ ...ctx, body, cfg, st, n });
}

/** Prev/next footer shared by steps. */
const nav = (n, nextLabel = 'Next') => html`
  <div class="btn-row" style="margin-top:20px">
    ${n > 1 ? html`<a class="btn back" href="#/setup/${n - 1}">${icon('back')}Back</a>` : ''}
    ${n < 4 ? html`<a class="btn primary" href="#/setup/${n + 1}" id="next">${nextLabel}${icon('next')}</a>` : html`<a class="btn primary" href="#/live">Done${icon('check')}</a>`}
  </div>`;

/** Seconds a car at the limit spends between the speed lines, from the current calibration. */
function zoneSeconds(cfg, speed) {
  const frames = Math.min(cfg.CalibrationFramesL2R, cfg.CalibrationFramesR2L) * cfg.CalibrationSpeed / speed;
  return frames / cfg.CalibrationFPS;
}

// ---- 1. camera & spot ---------------------------------------------------------------------------

function spot({ vst, body, cfg, st, n }) {
  const secs = zoneSeconds(cfg, st.speedLimit);
  // Short-view timing (subFrameTiming) times the crossing to a fraction of a frame, so a short zone is fine there.
  const short = !!cfg.subFrameTiming;
  const good = short || secs >= 1;
  body.innerHTML = html`
    <h1>Point the camera at the street</h1>
    <p class="lede">VST measures speed by timing each vehicle across a stretch of road, so the camera has to see that stretch side-on and steadily.</p>
    <div class="editor"><img src="${vst.frameUrl()}" alt="Current camera view" id="frame"></div>
    <div class="row between" style="margin-top:8px"><span class="small muted">This is what VST sees right now.</span>
      <button class="btn ghost" id="refresh">${icon('refresh')}Refresh</button></div>
    <h3>A good view has</h3>
    <div class="card">
      <div class="check">${icon('compass')}<p><b>The street crossing the picture side to side.</b> Aim straight across, not up or down the road; a window above street level is ideal.</p></div>
      <div class="check">${icon('ruler')}<p><b>Any clear stretch of road from about 14 ft.</b> A narrow street seen from a front window works, with short-view timing (one click in VST Studio's site planner). More road lets it time faster cars.</p></div>
      <div class="check">${icon('camera')}<p><b>Whatever camera you have, held still.</b> A webcam, a phone in webcam mode or an IP camera all work; 720p at 30 fps is plenty to start. A clamp or tripod keeps it from shifting.</p></div>
      <div class="check">${icon('sparkle')}<p><b>Little in the way.</b> A post or tree trunk is fine (you'll mark it), but avoid glare: put the lens against the glass and turn off room lights at night.</p></div>
    </div>
    <div class="callout" style="margin-top:12px">${icon(good ? 'check' : 'info')}
      <p>${short
        ? html`Short-view timing is on, so VST times each car to a fraction of a frame and a short speed zone is fine.`
        : good
        ? html`With the current calibration a car at ${st.speedLimit} mph spends <b>${secs.toFixed(1)} s</b> in the speed zone. That is enough for a reliable reading.`
        : html`A car at ${st.speedLimit} mph spends <b>${secs.toFixed(1)} s</b> in the speed zone, which is short for whole-frame timing. That's fine: turn on short-view timing in VST Studio's site planner (step 2, one click), or widen the zone in the next step.`}</p></div>
    ${nav(n, 'Looks good')}`.__html;
  $('#refresh').onclick = () => { $('#frame').src = vst.frameUrl(); };
}

// ---- 2. mark the street -------------------------------------------------------------------------

const PARTS = {
  tl: 'Drag the corners so the box covers just the road, from curb to curb. VST ignores everything outside it, like trees moving in the wind.',
  br: 'Drag the corners so the box covers just the road, from curb to curb. VST ignores everything outside it, like trees moving in the wind.',
  sl: 'The two white lines are where timing starts and stops. Put them as far apart as the view stays clear and level.',
  sr: 'The two white lines are where timing starts and stops. Put them as far apart as the view stays clear and level.',
  r2l: 'Put the orange line where the wheels of right-to-left traffic touch the road.',
  l2r: 'Put the purple line where the wheels of left-to-right traffic touch the road.',
  o0: 'Mark anything in front of the road, like a post or tree trunk, so VST knows vehicles disappear behind it.',
  o1: 'Mark anything in front of the road, like a post or tree trunk, so VST knows vehicles disappear behind it.',
};

function mark({ vst, body, cfg, n }) {
  const draft = { ...cfg, obstruction: [...(cfg.obstruction || [0, 0])] };
  let dirty = false;
  body.innerHTML = html`
    <h1>Mark the street</h1>
    <p class="lede">Drag the handles on the picture. Use two fingers to scroll the page if you need to.</p>
    <div class="editor" id="ed"><img src="${vst.frameUrl()}" alt="Camera view"><svg viewBox="0 0 ${W} ${H}" id="svg"></svg></div>
    <div class="legend">
      <span><i style="background:#fff"></i>Box &amp; speed lines</span>
      <span><i style="background:${LANE.R2L}"></i>Right-to-left wheels</span>
      <span><i style="background:${LANE.L2R}"></i>Left-to-right wheels</span>
    </div>
    <div class="callout" id="hint">${icon('info')}<p>Start with the box: drag its top-left and bottom-right corners to the edges of the road.</p></div>
    <div class="list" style="margin-top:12px">
      <div class="item"><div class="label"><b>Something blocks the view</b><small>A post, tree trunk or sign in front of the road</small></div>
        <label class="switch"><input type="checkbox" id="obs" aria-label="Something blocks the view" ${draft.obstruction[1] > draft.obstruction[0] ? 'checked' : ''}><i></i></label></div>
    </div>
    <p class="small muted" id="problems" style="margin-top:10px"></p>
    <div class="btn-row" style="margin-top:12px"><button class="btn" id="reset" disabled>Undo changes</button><button class="btn primary" id="save" disabled>${icon('check')}Save</button></div>
    ${nav(n)}`.__html;

  const svg = $('#svg'), ed = $('#ed');
  const k = () => W / (ed.clientWidth || 360);
  const draw = () => { svg.innerHTML = zonesSvg(draft, { k: k(), editable: true }); };
  const setDirty = (d) => { dirty = d; $('#save').disabled = !d; $('#reset').disabled = !d; };

  // Pointer drags in frame coordinates; one active handle at a time.
  let active = null;
  const toFrame = (e) => {
    const r = svg.getBoundingClientRect();
    return [(e.clientX - r.left) / r.width * W, (e.clientY - r.top) / r.height * H];
  };
  svg.addEventListener('pointerdown', (e) => {
    const h = e.target.closest('[data-h]')?.dataset.h;
    if (!h) return;
    active = h;
    svg.setPointerCapture(e.pointerId);
    $('#hint p').textContent = PARTS[h];
    e.preventDefault();
  });
  svg.addEventListener('pointermove', (e) => {
    if (!active) return;
    Object.assign(draft, applyHandle(draft, active, ...toFrame(e)));
    setDirty(true);
    draw();
  });
  const end = () => { active = null; };
  svg.addEventListener('pointerup', end);
  svg.addEventListener('pointercancel', end);

  $('#obs').onchange = (e) => {
    const g = geometry(draft);
    // A fresh obstruction starts as a narrow band in the middle of the speed zone, ready to drag.
    const mid = (g.sl + g.sr) / 2 - draft.AnalysisBoxLeft;
    draft.obstruction = e.target.checked ? [Math.round(mid - 30), Math.round(mid + 30)] : [0, 0];
    setDirty(true);
    draw();
  };
  $('#reset').onclick = () => { Object.assign(draft, cfg, { obstruction: [...(cfg.obstruction || [0, 0])] }); $('#obs').checked = draft.obstruction[1] > draft.obstruction[0]; setDirty(false); draw(); };
  $('#save').onclick = async () => {
    const keys = ['AnalysisBoxLeft', 'AnalysisBoxTop', 'AnalysisBoxWidth', 'AnalysisBoxHeight', 'speedLineLeft', 'speedLineRight', 'obstruction', 'R2LStreetY', 'L2RStreetY'];
    const r = await vst.putConfig(Object.fromEntries(keys.map((key) => [key, draft[key]])));
    $('#problems').textContent = r.ok ? '' : r.problems.join('. ');
    // VST saves at once, but a run in progress keeps the zones it started with (docs/companion-api.md).
    if (r.ok) { Object.assign(cfg, r.config); setDirty(false); toast(vst.lastStatus?.running ? 'Saved. VST uses the new zones from the next start.' : 'Saved. VST will use the new zones.'); }
  };
  $('#next').addEventListener('click', async (e) => {
    if (!dirty) return;
    e.preventDefault();
    if (await confirmSheet('Leave without saving?', 'Your changes to the zones will be lost.', 'Leave', true)) location.hash = '#/setup/3';
  });

  const onResize = () => draw();
  window.addEventListener('resize', onResize);
  requestAnimationFrame(draw);
  return () => window.removeEventListener('resize', onResize);
}

// ---- 3. rules of the road -----------------------------------------------------------------------

/** Tiny street illustration for the direction choices. */
const street = (legal) => {
  const arrows = { R2L: [['R2L', 26]], L2R: [['L2R', 26]], both: [['R2L', 18], ['L2R', 34]] }[legal];
  return html`<svg class="street" viewBox="0 0 88 52" aria-hidden="true">
    <rect width="88" height="52" rx="10" fill="var(--surface-3)"/><rect y="12" width="88" height="28" fill="#2a2f37"/>
    ${legal === 'both' ? html`<line x1="6" x2="82" y1="26" y2="26" stroke="#fbbf24" stroke-width="1.5" stroke-dasharray="5 4"/>` : ''}
    ${arrows.map(([d, y]) => html`<path d="${d === 'L2R' ? `M24 ${y}H62M55 ${y - 6}l7 6-7 6` : `M64 ${y}H26M33 ${y - 6}l-7 6 7 6`}" fill="none" stroke="#5eead4" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/>`)}
  </svg>`;
};

function rules({ vst, body, cfg, n }) {
  const d = { legalDirection: cfg.legalDirection, L2RDirection: cfg.L2RDirection, R2LDirection: cfg.R2LDirection,
    speedLimit: cfg.speedLimit, egregious: cfg.egregious, siteName: cfg.siteName || '' };
  body.innerHTML = html`
    <h1>Rules of the road</h1>
    <p class="lede">Tell VST which way traffic is allowed to go and how fast, so it can flag speeders and wrong-way drivers.</p>
    <h3>Which way may traffic go?</h3>
    <div class="choice" id="legal">
      <button data-v="R2L">${street('R2L')}<span><b>One-way, right to left</b><small>Traffic going left to right is flagged as wrong way</small></span></button>
      <button data-v="L2R">${street('L2R')}<span><b>One-way, left to right</b><small>Traffic going right to left is flagged as wrong way</small></span></button>
      <button data-v="both">${street('both')}<span><b>Two-way street</b><small>Speeds only, both directions</small></span></button>
    </div>
    <h3>Compass headings</h3>
    <p class="small">Used in the stats file and on cards. Check a map: which way does traffic head when it moves across your screen?</p>
    <div class="row">
      <label class="field grow"><span>Moving ← right to left</span><select class="input" id="r2l">${COMPASS.map((c) => html`<option ${c === d.R2LDirection ? 'selected' : ''}>${c}</option>`)}</select></label>
      <label class="field grow"><span>Moving → left to right</span><select class="input" id="l2r">${COMPASS.map((c) => html`<option ${c === d.L2RDirection ? 'selected' : ''}>${c}</option>`)}</select></label>
    </div>
    <h3>Speeds</h3>
    <div class="row">
      <div class="field grow"><span>Posted limit</span><div class="stepper" data-k="speedLimit"><button data-d="-5" aria-label="Lower the limit">−</button><output aria-live="polite"></output><button data-d="5" aria-label="Raise the limit">+</button></div></div>
      <div class="field grow"><span>"Way too fast" from</span><div class="stepper" data-k="egregious"><button data-d="-1" aria-label="Lower">−</button><output aria-live="polite"></output><button data-d="1" aria-label="Higher">+</button></div></div>
    </div>
    <h3>Name for this spot</h3>
    <label class="field"><input class="input" id="site" aria-label="Name for this spot" maxlength="40" placeholder="e.g. 7½ St SW" value="${d.siteName}"><span class="small muted" style="font-weight:400">Shown in the app, and on shared cards only if you allow it.</span></label>
    <p class="small muted" id="problems" style="margin-top:10px"></p>
    ${nav(n, 'Save and continue')}`.__html;

  const paint = () => {
    $$('#legal button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.v === d.legalDirection));
    $$('.stepper').forEach((s) => { s.querySelector('output').textContent = `${d[s.dataset.k]} mph`; });
  };
  $('#legal').onclick = (e) => { const v = e.target.closest('[data-v]')?.dataset.v; if (v) { d.legalDirection = v; paint(); } };
  $$('.stepper').forEach((s) => s.addEventListener('click', (e) => {
    const step = Number(e.target.closest('[data-d]')?.dataset.d || 0);
    if (!step) return;
    d[s.dataset.k] = Math.max(5, Math.min(90, d[s.dataset.k] + step));
    if (s.dataset.k === 'speedLimit' && d.egregious <= d.speedLimit) d.egregious = d.speedLimit + 10;
    paint();
  }));
  $('#next').addEventListener('click', async (e) => {
    e.preventDefault();
    Object.assign(d, { R2LDirection: $('#r2l').value, L2RDirection: $('#l2r').value, siteName: $('#site').value.trim() });
    const r = await vst.putConfig(d);
    if (!r.ok) { $('#problems').textContent = r.problems.join('. '); return; }
    await vst.status();
    location.hash = '#/setup/4';
  });
  paint();
}

// ---- 4. calibration drive, then a check drive ---------------------------------------------------

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
    <h1>Calibration drive</h1>
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
        <div class="check"><b class="num" style="color:var(--accent)">b</b><p><b>Tap each spot</b> on the live picture below. Any point on an upright pole will do, so one person can tap spot 1, walk the pole to spot 2 and tap again.</p></div>
        <div class="check"><b class="num" style="color:var(--accent)">c</b><p><b>Enter the taped distance</b> and apply. Then drive past once to check it when you can.</p></div>
        <div class="editor" id="ced" style="margin-top:10px"><span id="clive"></span><svg viewBox="0 0 ${W} ${H}" id="csvg"></svg></div>
        <div class="seg" id="cwhich" style="margin-top:10px"><button data-v="0" aria-pressed="true">Spot 1</button><button data-v="1">Spot 2</button></div>
        <label class="field" style="margin-top:10px"><span>Spots apart (ft)</span><input class="input" id="cft" type="number" inputmode="decimal" min="1" step="0.5" value="10"></label>
        <div class="seg" id="cdir" style="margin-top:10px"><button data-v="both" aria-pressed="true">Both ways</button><button data-v="R2L">← only</button><button data-v="L2R">→ only</button></div>
        <p class="small muted" id="cprev" style="margin-top:10px"></p>
        <div class="btn-row"><button class="btn" id="cclear">Clear spots</button><button class="btn primary" id="capply" disabled>Apply</button></div>
      </div>
    </details>
    <div id="phase"></div>
    <h3>Speed you will drive</h3>
    <div class="stepper" style="max-width:240px"><button data-d="-1" aria-label="Slower">−</button><output id="spd" aria-live="polite"></output><button data-d="1" aria-label="Faster">+</button></div>
    <div class="section-head"><h3>Passes since you opened this</h3><span class="pill rec small" id="listening"><i style="width:7px;height:7px;border-radius:50%;background:var(--ok)"></i>Listening</span></div>
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
   * selection to the other one. The preview repeats the server's arithmetic (Calibration.h framesForDistance) so the
   * zone length and frames are visible before applying.
   */
  const cones = [null, null];
  let cone = 0, cdir = 'both';
  const csvg = $('#csvg');
  const paintCones = () => {
    const g = geometry(current), k = W / ($('#ced').clientWidth || 360);
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
  csvg.addEventListener('pointerdown', (e) => {
    const r = csvg.getBoundingClientRect();
    cones[cone] = [Math.round((e.clientX - r.left) / r.width * W), Math.round((e.clientY - r.top) / r.height * H)];
    if (!cones[1 - cone]) cone = 1 - cone;
    paintCones();
    e.preventDefault();
  });
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

  paintSpeed(); paintCal(cfg, st.calibration); paintPhase(); paintPasses();
  return () => { vst.removeEventListener('vehicle', onVehicle); stopLive(); };
}
