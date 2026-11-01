/**
 * Setup: a four-step guided calibration a first-time user can follow from the phone while standing
 * at the camera (or out on the street for the calibration drive).  The kicker over each page names the step by
 * VST Studio's numbering (DESKTOP below), so both apps say "step 5" for calibrating.
 *
 *   1. Camera & spot   - what a good view looks like, checked against the live frame, and "Your street": VST Studio's
 *                        site planner (GET/PUT /api/plan), starting from a rough guess and sized from the picture by
 *                        sliding a car-length ruler over it; a calibration (page 4) measures it for real;
 *   2. Mark the street - drag the analysis box, speed lines, lane lines and obstruction on the frame;
 *   3. Rules           - which way is legal, compass headings, limit, "way too fast", site name;
 *   4. Calibration drive - drive through at a known speed, tap your pass, then drive once more to check it;
 *      or, with no car handy, tap two spots a measured distance apart (cones, or one pole moved between them).
 *
 * Pages 2 and 3 edit a draft and save it with PUT /api/config; VST validates and the problems it
 * reports are shown in plain words. Page 4 uses POST /api/calibration/apply with the chosen pass,
 * then POST /api/calibration/check with a second one.
 */
import { html, $, $$, icon, sign, band, arrow, clock, ago, toast, sheet, confirmSheet } from '../ui.js';
import { zonesSvg, applyHandle, handlePoint, HANDLE_AXES, geometry, W, H, LANE } from '../overlay.js';
import { frameStage, nudgePad, carRuler } from '../touch.js';

const STEPS = ['Camera & spot', 'Mark the street', 'Rules of the road', 'Calibration drive'];
// The same steps by VST Studio's numbering (1 What you need … 5 Calibrate), so the phone and the desktop always name a
// step the same way; the phone's four pages are only how it is split up for a small screen.
const DESKTOP = ['Steps 2–3 · Pick your spot, aim the camera', 'Step 4 · Mark your street', 'Step 4 · Mark your street: rules of the road', 'Step 5 · Calibrate'];
const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

export async function render(ctx, step = '1') {
  const n = Math.min(4, Math.max(1, Number(step)));
  const cfg = await ctx.vst.config();
  const st = await ctx.vst.status();
  ctx.view.innerHTML = html`
    <div class="step-kicker">${DESKTOP[n - 1]}</div>
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
    <h3>Your street</h3>
    <div class="card" id="plan"><p class="small muted">Loading the plan…</p></div>
    <div class="callout" style="margin-top:12px">${icon(good ? 'check' : 'info')}
      <p>${short
        ? html`Short-view timing is on, so VST times each car to a fraction of a frame and a short speed zone is fine.`
        : good
        ? html`With the current calibration a car at ${st.speedLimit} mph spends <b>${secs.toFixed(1)} s</b> in the speed zone. That is enough for a reliable reading.`
        : html`A car at ${st.speedLimit} mph spends <b>${secs.toFixed(1)} s</b> in the speed zone, which is short for whole-frame timing. That's fine: turn on short-view timing in VST Studio's site planner (step 2, one click), or widen the zone in the next step.`}</p></div>
    ${nav(n, 'Looks good')}`.__html;
  $('#refresh').onclick = () => { $('#frame').src = vst.frameUrl(); };
  planCard(vst, $('#plan'));   // loads on its own, so the step shows at once
}

const PILL = { guess: 'warn', typed: '', picture: '', calibration: 'ok' };
const GRADE = ['ok', 'warn', 'bad'];

/**
 * "Your street": the planner's verdict and where its numbers came from, the presets, a car-length ruler over its own
 * zoomable still from the camera (touch.js carRuler: drag the bright block onto a car, its ends to the bumpers, then
 * "Use it"), the measured numbers when a calibration
 * has them, and the two lengths for anyone who has measured.  Every change goes to VST (PUT /api/plan), which
 * answers with the whole plan, so the card always shows what the desktop shows.
 */
async function planCard(vst, el) {
  let p;
  try { p = await vst.plan(); } catch { el.innerHTML = html`<p class="small muted">This VST doesn't have the site planner yet. Update VST Studio to plan from the phone.</p>`.__html; return; }
  let n = p.carLengths ?? Math.min(20, Math.max(1, Math.round(p.street.coverFt / p.typicalCarFt * 2) / 2));
  // touch.js carRuler, rebuilt with the card; n and where the block sits survive a re-render (e.g. after "Use it").
  let ruler = null, rpos = null;

  const render = () => {
    const v = p.verdict, st = p.street;
    el.innerHTML = html`
      <div class="row" style="gap:8px;flex-wrap:wrap"><span class="pill ${GRADE[v.grade]}">${v.label}</span><span class="pill ${PILL[st.source]}">${st.sourceName}</span></div>
      <p style="margin:8px 0 4px">${v.text}</p>
      ${v.fastest ? html`<p class="small muted" style="margin:0">At ${p.camera.fps} fps this view times cars ${v.fastest}${p.camera.fps < 59 && v.fastest60 !== v.fastest ? html`; at 60 fps, ${v.fastest60}` : ''}.</p>` : ''}
      ${v.frameRate ? html`<p class="small" style="margin:6px 0 0;color:var(--warn)">${v.frameRate}</p>` : ''}
      <p class="small muted" style="margin:6px 0 0">${st.source === 'guess' ? 'A rough guess is fine to start; zero in below.' : st.note || 'As you typed them.'}
        ${st.distFt.toFixed(0)} ft to the lane, ${st.clearFt.toFixed(0)} ft clear, about ${st.coverFt.toFixed(0)} ft across the picture.</p>

      <h4 style="margin:16px 0 8px">Which is closest to your view?</h4>
      <div class="choice" id="presets">${p.presets.map((x) => html`<button data-k="${x.key}" aria-pressed="${st.source === 'guess' && Math.abs(st.distFt - x.distFt) < .5 && Math.abs(st.clearFt - x.clearFt) < .5}"><span><b>${x.name}</b><small>${x.hint}</small></span></button>`)}</div>

      <h4 style="margin:16px 0 8px">Size it from the picture</h4>
      <div id="ruler"></div>

      ${p.measured ? html`
        <div class="callout ok" style="margin-top:14px">${icon('check')}<p>Measured by ${p.measured.how}: ${p.measured.coverFt.toFixed(0)} ft across the picture, ${p.measured.clearFt.toFixed(0)} ft of it clear.
          ${st.source !== 'calibration' ? html`<br><button class="btn" id="use-measured" style="margin-top:8px">Use the measured numbers</button>` : ''}</p></div>`
      : html`<p class="small muted" style="margin:14px 0 0">Calibrating (step 5: a drive past, or cones or a pole) measures the street exactly, and this updates by itself.</p>`}

      <details style="margin-top:14px"><summary class="small">I've measured it</summary>
        <div class="row" style="gap:10px;margin-top:10px">
          <label class="field grow"><span>Window to the lane (ft)</span><input class="input" id="dist" type="number" inputmode="decimal" min="3" max="1000" value="${st.distFt.toFixed(0)}"></label>
          <label class="field grow"><span>Clear stretch (ft)</span><input class="input" id="clear" type="number" inputmode="decimal" min="3" max="1000" value="${st.clearFt.toFixed(0)}"></label>
        </div>
        <button class="btn" id="save-typed" style="margin-top:10px">${icon('check')}Save</button>
        <p class="small muted" style="margin:8px 0 0">No tape? One easy step is about 2.5 ft, sidewalk slabs are usually 5 ft, a parked car about 15 ft.</p>
      </details>
      <p class="small" id="plan-problems" style="color:var(--bad)"></p>`.__html;
    ruler?.destroy();
    ruler = carRuler($('#ruler', el), {
      frameUrl: () => vst.frameUrl(), n, at: rpos, carFt: p.typicalCarFt,
      onChange: (v, pos) => { n = v; rpos = pos; },
      onUse: (v) => send({ carLengths: v }, 'Planned from your picture. A calibration will measure it exactly.'),
    });
    const send = async (change, said) => {
      const r = await vst.putPlan(change);
      p = r.plan;
      if (!r.ok) { $('#plan-problems', el).textContent = r.problems.join('. '); return; }
      render();
      if (said) toast(said);
    };
    $('#presets', el).onclick = (e) => { const k = e.target.closest('[data-k]')?.dataset.k; if (k) send({ preset: k }); };
    const um = $('#use-measured', el);
    if (um) um.onclick = () => send({ useMeasured: true });
    $('#save-typed', el).onclick = () => send({ distFt: Number($('#dist', el).value), clearFt: Number($('#clear', el).value) }, 'Saved your numbers.');
  };
  render();
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

/** The parts the phone can pick and move, in the order a novice sets them; names match PARTS' words. */
const PICK = [
  ['tl', 'Box corner ↖', '#fff'], ['br', 'Box corner ↘', '#fff'], ['sl', 'Start line', '#fff'], ['sr', 'End line', '#fff'],
  ['r2l', '← Wheels', LANE.R2L], ['l2r', 'Wheels →', LANE.L2R], ['o0', 'Post, left edge', '#cbd5e1'], ['o1', 'Post, right edge', '#cbd5e1'],
];

/**
 * Mark the street. On a phone the frame is ~220 px tall, so editing works three ways, all on one selected part:
 *   - drag on the picture: grab near a handle (the nearest one within ~32 px of the finger) and it moves by the
 *     finger's travel, with a loupe showing it magnified; pinch, double-tap or + to zoom in first;
 *   - pick a part from the chips under the picture (no need to hit a 9 px dot at all);
 *   - the arrow pad moves the picked part one frame pixel per tap, faster when held.
 * touch.js does the gestures and zoom; this view only maps a handle and a frame point to config through applyHandle.
 */
function mark({ vst, body, cfg, n }) {
  const draft = { ...cfg, obstruction: [...(cfg.obstruction || [0, 0])] };
  let dirty = false;
  let sel = 'tl';
  body.innerHTML = html`
    <h1>Mark the street</h1>
    <p class="lede">Drag the handles on the picture, or pick a part below and use the arrows. Pinch or tap + to zoom in for a precise fit.</p>
    <div class="editor" id="ed"><img src="${vst.frameUrl()}" alt="Camera view"><svg viewBox="0 0 ${W} ${H}" id="svg"></svg></div>
    <div class="parts" id="parts" role="group" aria-label="Part to move"></div>
    <div class="fine">
      <div class="grow"><b id="sel-name"></b><small id="sel-pos"></small></div>
      <div id="pad"></div>
    </div>
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

  const setDirty = (d) => { dirty = d; $('#save').disabled = !d; $('#reset').disabled = !d; };
  const hasObs = () => draft.obstruction[1] > draft.obstruction[0];
  const parts = () => PICK.filter(([id]) => hasObs() || !id.startsWith('o'));
  const paintParts = () => {
    $('#parts').innerHTML = parts().map(([id, name, col]) => html`<button type="button" data-h="${id}" aria-pressed="${id === sel}"><i style="background:${col}"></i>${name}</button>`.__html).join('');
  };
  /** The picked part's name, where it is, and the matching hint; arrows that can't move it are disabled. */
  const paintSel = () => {
    const name = PICK.find(([id]) => id === sel)?.[1] || '';
    const pt = handlePoint(draft, sel, ed.k());
    $('#sel-name').textContent = name;
    $('#sel-pos').textContent = pt ? `${HANDLE_AXES[sel].includes('x') ? `x ${Math.round(pt[0])}` : ''}${HANDLE_AXES[sel] === 'xy' ? ' · ' : ''}${HANDLE_AXES[sel].includes('y') ? `y ${Math.round(pt[1])}` : ''} of ${W}×${H}` : '';
    pad.setAxes(HANDLE_AXES[sel]);
    $('#hint p').textContent = PARTS[sel];
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
    Object.assign(draft, applyHandle(draft, h, pt[0], pt[1], ed.k()));
    setDirty(true);
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
    setDirty(true);
    paintParts();
    select(e.target.checked ? 'o0' : sel.startsWith('o') ? 'tl' : sel);
  };
  $('#reset').onclick = () => { Object.assign(draft, cfg, { obstruction: [...(cfg.obstruction || [0, 0])], ...(cfg.streetEdge ? { streetEdge: [...cfg.streetEdge] } : {}) }); $('#obs').checked = hasObs(); setDirty(false); paintParts(); select(hasObs() || !sel.startsWith('o') ? sel : 'tl'); };
  $('#save').onclick = async () => {
    const keys = ['AnalysisBoxLeft', 'AnalysisBoxTop', 'AnalysisBoxWidth', 'AnalysisBoxHeight', 'speedLineLeft', 'speedLineRight', 'obstruction',
      'R2LStreetY', 'L2RStreetY', 'R2LStreetYRight', 'L2RStreetYRight', 'streetEdge'].filter((key) => key in draft);
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

  return () => ed.destroy();
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
  return () => { vst.removeEventListener('vehicle', onVehicle); stopLive(); cst.destroy(); };
}
