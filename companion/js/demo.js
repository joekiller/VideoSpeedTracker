/**
 * Simulated VST that implements the companion API (docs/companion-api.md) without a camera.
 *
 * One module, two transports:
 *   - the browser's demo mode calls `handle()` and `subscribe()` directly (js/api.js), so the app can be
 *     explored from any static host;
 *   - dev/mock-vst.mjs wraps the same calls in a real HTTP server, so VST Studio's implementation and the
 *     companion can each be tested against the contract alone.
 *
 * Everything here is plain data: no DOM, no Node APIs. Binary endpoints answer `{ file }` naming an asset
 * under companion/demo/, which the caller turns into a URL (browser) or streams from disk (Node).
 *
 * Traffic is generated, not recorded: two weeks of passes on a one-way street whose legal direction is
 * right-to-left, with a weekday rush-hour profile, speeds around the limit, and the odd wrong-way driver.
 * The pictures come from the original VST sample video (Locust Avenue, 2016).
 */

import * as plan from './plan.js';
import * as calib from './calib.js';
// The narrowest speed zone VST accepts (Globals::kMinZone; overlay.js MIN_ZONE). Kept here so the demo needs no DOM module.
const MIN_ZONE = 200;

const SNAPSHOTS = 6;
const DIRS = ['L2R', 'R2L'];
const DAY = 86400e3;

/** Small deterministic PRNG (mulberry32) so the demo looks the same on every load. */
function rng(seed) {
  return () => {
    seed |= 0; seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** Normal deviate via Box-Muller from a uniform source. */
function gauss(rand) {
  return Math.sqrt(-2 * Math.log(rand() || 1e-9)) * Math.cos(2 * Math.PI * rand());
}

/** Relative traffic per hour of day: quiet nights, school-run and commute peaks. */
const HOURLY = [0.2, 0.1, 0.1, 0.1, 0.2, 0.5, 1.5, 3.2, 4.0, 2.6, 2.0, 2.2,
  2.6, 2.4, 2.4, 3.0, 3.8, 4.2, 3.2, 2.2, 1.6, 1.1, 0.7, 0.4];

/** ISO-8601 with the browser/Node local offset, like VST Studio writes it. */
function localIso(ms) {
  const d = new Date(ms);
  const off = -d.getTimezoneOffset();
  const pad = (n) => String(Math.floor(Math.abs(n))).padStart(2, '0');
  const local = new Date(ms + off * 60e3).toISOString().slice(0, 19);
  return `${local}${off >= 0 ? '+' : '-'}${pad(off / 60)}:${pad(off % 60)}`;
}

export function createDemoVst({ now = () => Date.now(), tracks = true } = {}) {
  const rand = rng(7_1_2);
  const started = now();

  const config = {
    AnalysisBoxLeft: 10, AnalysisBoxTop: 220, AnalysisBoxWidth: 1269, AnalysisBoxHeight: 190,
    speedLineLeft: 350, speedLineRight: 909,
    obstruction: [251, 311],
    R2LStreetY: 122, L2RStreetY: 158, R2LStreetYRight: 0, L2RStreetYRight: 0, streetEdge: [0, 0],
    L2RDirection: 'SW', R2LDirection: 'NE',
    CalibrationFramesL2R: 35, CalibrationFramesR2L: 40, CalibrationSpeed: 25, CalibrationFPS: 30,
    legalDirection: 'R2L',
    speedLimit: 25, egregious: 40, siteName: 'Demo Street',
    // Reported by VST Studio too (docs: "VST Studio also reports"); the companion doesn't edit them.
    obstruction_extent: 30, SENSITIVITY_VALUE: 30, BLUR_SIZE: 20, largeVehicleArea: 100, minLoggedSpeed: 18,
    frameWidth: 1280, frameHeight: 720,
  };
  const state = { running: true, paused: false, frame: 0, marks: {} };
  // Step 2's site planner (GET/PUT /api/plan), with VST Studio's draft model (src/Gui/Guide.cpp planCommit): every
  // change goes into a draft the plan previews, and only {"save": true} (or autosave) makes it the saved plan; the
  // one-way, compass and site name the map suggests wait in `pending` until then too.  The demo street is calibrated
  // (the Locust Avenue numbers above), so it starts as VST Studio would after step 5: measured.
  const camera = { hfov: 66, width: 1920, fps: 30, shutter: 120 };
  const measuredPlan = () => {
    const c = config;
    const frames = c.legalDirection === 'L2R' ? c.CalibrationFramesL2R : c.legalDirection === 'R2L' ? c.CalibrationFramesR2L
      : (c.CalibrationFramesL2R + c.CalibrationFramesR2L) / 2;
    return plan.fromCalibration(frames, c.CalibrationSpeed, c.CalibrationFPS, c.speedLineRight - c.speedLineLeft, c.AnalysisBoxWidth,
      Math.max(0, c.obstruction[1] - c.obstruction[0]));
  };
  const measuredStreet = () => {
    const m = measuredPlan();
    return { distFt: plan.distForCoverFt(m.coverFt, camera.hfov), clearFt: m.clearFt, source: 'calibration', note: 'Measured by your step 5 calibration' };
  };
  let saved = { ...measuredStreet(), carLengths: null, map: null };
  let draft = { ...saved };
  let pending = {};   // { shortView, legalDirection, L2RDirection, R2LDirection, siteName } applied to config on save
  let autosave = false;
  const r1 = (v) => Math.round(v * 10) / 10;
  const same = (x, y) => Math.abs(x.distFt - y.distFt) < .05 && Math.abs(x.clearFt - y.clearFt) < .05 && x.source === y.source
    && JSON.stringify(x.map) === JSON.stringify(y.map) && (x.speedLimit ?? config.speedLimit) === (y.speedLimit ?? config.speedLimit);
  const dirty = () => !same(draft, saved) || Object.keys(pending).length > 0;
  /** The draft's changes in words, like App::planChanges. */
  const changes = () => {
    const out = [];
    const len = (what, was, now) => { if (Math.abs(was - now) >= .5) out.push(`${what}: ${was.toFixed(0)} → ${now.toFixed(0)} ft`); };
    len('Window to the lane', saved.distFt, draft.distFt);
    len('Clear stretch', saved.clearFt, draft.clearFt);
    if (draft.source !== saved.source) out.push(`Street numbers: ${plan.SOURCE_NAMES[saved.source]} → ${plan.SOURCE_NAMES[draft.source]}`);
    const lim = draft.speedLimit ?? config.speedLimit;
    if (lim !== config.speedLimit) out.push(`Speed limit: ${config.speedLimit} → ${lim} mph`);
    if (JSON.stringify(draft.map) !== JSON.stringify(saved.map)) out.push(draft.map?.street ? `Map: camera and lane pins on ${draft.map.street}` : 'Map: camera and lane pins placed');
    if (pending.shortView) out.push('Turn on short-view timing (step 4\'s settings)');
    if (pending.legalDirection) out.push(`One-way street: legal direction ${pending.legalDirection === 'L2R' ? 'left to right' : 'right to left'} in the picture`);
    if (pending.L2RDirection) out.push(`Compass: left to right is ${pending.L2RDirection}-bound, right to left ${pending.R2LDirection}-bound`);
    if (pending.siteName) out.push(`Site name: ${config.siteName} → ${pending.siteName}`);
    return out;
  };
  const commit = () => {
    if (draft.speedLimit != null) {
      if (plan.minLoggedFollows(config.minLoggedSpeed, config.speedLimit)) config.minLoggedSpeed = plan.minLoggedForLimit(draft.speedLimit);
      config.speedLimit = draft.speedLimit;
      if (config.egregious <= config.speedLimit) config.egregious = config.speedLimit + 10;
    }
    if (pending.shortView) config.subFrameTiming = 1;
    for (const k of ['legalDirection', 'L2RDirection', 'R2LDirection', 'siteName']) if (pending[k]) config[k] = pending[k];
    delete draft.speedLimit;
    saved = { ...draft };
    pending = {};
  };
  const edited = () => { if (autosave) commit(); };
  const verdictOf = (st, short) => plan.verdict({ ...camera, distFt: st.distFt, clearFt: st.clearFt, speedLimit: st.speedLimit ?? config.speedLimit }, short);
  function planJson() {
    const lim = draft.speedLimit ?? config.speedLimit;
    const m = measuredPlan();
    const v = verdictOf(draft, !!config.subFrameTiming || !!pending.shortView);
    const sv = verdictOf(saved, !!config.subFrameTiming);
    let map = null;
    if (draft.map) {
      const r = plan.fromMap(draft.map);
      map = { camLat: draft.map.camLat, camLon: draft.map.camLon, laneLat: draft.map.laneLat, laneLon: draft.map.laneLon,
        street: draft.map.street || '', distFt: r ? r1(r.distFt) : 0, facing: r ? r.facing : '' };
    }
    return {
      street: { distFt: r1(draft.distFt), clearFt: r1(draft.clearFt), coverFt: r1(plan.coverFt(draft.distFt, camera.hfov)), source: draft.source,
        sourceName: plan.SOURCE_NAMES[draft.source], note: draft.note, speedLimit: lim },
      presets: plan.PRESETS.map((x) => ({ ...x })),
      camera: { ...camera },
      verdict: { ...v, frameRate: null },
      measured: m ? { coverFt: r1(m.coverFt), clearFt: r1(m.clearFt), how: 'your step 5 calibration' } : null,
      carLengths: draft.carLengths,
      picture: true,
      typicalCarFt: plan.TYPICAL_CAR_FT,
      map,
      legalDirection: pending.legalDirection || config.legalDirection,
      L2RDirection: pending.L2RDirection || config.L2RDirection, R2LDirection: pending.R2LDirection || config.R2LDirection,
      shortViewOnSave: !!pending.shortView,
      dirty: dirty(), changes: changes(), autosave,
      saved: { distFt: r1(saved.distFt), clearFt: r1(saved.clearFt), speedLimit: config.speedLimit, source: saved.source,
        grade: sv.grade, label: sv.label, text: sv.text },
    };
  }
  /** Like VST Studio's applyPlan (src/Gui/Server.cpp): one change per request, problems in words. */
  function setPlan(b) {
    const num = (k) => (k in b ? (typeof b[k] === 'number' ? b[k] : NaN) : undefined);
    const street = (distFt, clearFt, source, note) => Object.assign(draft, { distFt: Math.max(distFt, 3), clearFt: Math.max(clearFt, 3), source, note });
    if ('save' in b) { if (b.save === true) commit(); return []; }
    if ('discard' in b) { if (b.discard === true) { draft = { ...saved }; pending = {}; } return []; }
    if ('autosave' in b) {
      if (typeof b.autosave !== 'boolean') return ['autosave must be true or false'];
      autosave = b.autosave;
      if (autosave && dirty()) commit();
      return [];
    }
    if ('preset' in b) {
      const x = plan.PRESETS.find((q) => q.key === b.preset);
      if (!x) return ['unknown preset'];
      street(x.distFt, x.clearFt, 'guess', x.name);
    } else if ('carLengths' in b) {
      const n = num('carLengths');
      if (!(n >= 1 && n <= 20)) return ['carLengths must be between 1 and 20'];
      const m = plan.fromCarLengths(n, config.AnalysisBoxWidth, Math.max(0, config.obstruction[1] - config.obstruction[0]));
      street(plan.distForCoverFt(m.coverFt, camera.hfov), m.clearFt, 'picture', `${n.toFixed(1)} car lengths across the picture`);
      draft.carLengths = n;
    } else if (b.useMeasured) {
      Object.assign(draft, measuredStreet());
    } else if ('shortView' in b) {
      if (typeof b.shortView !== 'boolean') return ['shortView must be true or false'];
      if (b.shortView) pending.shortView = true; else delete pending.shortView;
    } else if ('map' in b) {
      const mj = b.map;
      if (!mj || typeof mj !== 'object' || Array.isArray(mj)) return ['map must be an object'];
      for (const k of ['camLat', 'camLon', 'laneLat', 'laneLon']) if (k in mj && typeof mj[k] !== 'number') return [`map.${k} must be a number`];
      if (![mj.camLat, mj.laneLat].every((x) => x == null || (x >= -90 && x <= 90))) return ['latitudes must be between -90 and 90'];
      if (![mj.camLon, mj.laneLon].every((x) => x == null || (x >= -180 && x <= 180))) return ['longitudes must be between -180 and 180'];
      const pick = { camLat: mj.camLat || 0, camLon: mj.camLon || 0, laneLat: mj.laneLat || 0, laneLon: mj.laneLon || 0,
        ...(typeof mj.wayBearing === 'number' ? { wayBearing: mj.wayBearing } : {}),
        oneway: typeof mj.oneway === 'number' ? Math.max(-1, Math.min(1, Math.round(mj.oneway))) : 0,
        maxspeedMph: typeof mj.maxspeedMph === 'number' ? mj.maxspeedMph : 0,
        street: typeof mj.street === 'string' ? mj.street.slice(0, 80) : '' };
      const r = plan.fromMap(pick);
      if (!r) {
        const d = pick.camLat && pick.laneLat ? plan.groundDistanceM(pick.camLat, pick.camLon, pick.laneLat, pick.laneLon) * plan.FT : 0;
        return [d ? `the two pins are ${d.toFixed(0)} ft apart; put one on your window and one in the middle of the lane in front of it`
          : 'place both pins: your window, then the middle of the lane'];
      }
      street(r.distFt, Math.min(draft.clearFt, plan.coverFt(r.distFt, camera.hfov)), 'map', `Measured on the map${pick.street ? `, ${pick.street}` : ''}`);
      draft.map = { camLat: pick.camLat, camLon: pick.camLon, laneLat: pick.laneLat, laneLon: pick.laneLon, street: pick.street };
      if (r.speedLimit) draft.speedLimit = r.speedLimit;
      pending = { ...(pending.shortView ? { shortView: true } : {}) };
      if (r.l2r !== config.L2RDirection || r.r2l !== config.R2LDirection) Object.assign(pending, { L2RDirection: r.l2r, R2LDirection: r.r2l });
      if (r.legal !== 'both' && r.legal !== config.legalDirection) pending.legalDirection = r.legal;
      if (pick.street && (!config.siteName || config.siteName === 'My street')) pending.siteName = pick.street;
    } else {
      const d = num('distFt'), c = num('clearFt'), lim = num('speedLimit');
      if (d === undefined && c === undefined && lim === undefined) return ['send preset, distFt/clearFt, speedLimit, carLengths, useMeasured, shortView, map, save, discard or autosave'];
      if (d !== undefined && !(d >= 3 && d <= 1000)) return [Number.isNaN(d) ? 'distFt must be a number' : 'distFt must be between 3 and 1000'];
      if (c !== undefined && !(c >= 3 && c <= 1000)) return [Number.isNaN(c) ? 'clearFt must be a number' : 'clearFt must be between 3 and 1000'];
      if (lim !== undefined && !(lim >= 5 && lim <= 80)) return [Number.isNaN(lim) ? 'speedLimit must be a number' : 'speedLimit must be between 5 and 80'];
      if (d !== undefined || c !== undefined) street(d ?? draft.distFt, c ?? draft.clearFt, 'typed', '');
      if (lim !== undefined) draft.speedLimit = Math.round(lim);
    }
    edited();
    return [];
  }
  // VST Studio's calibration-history.csv, in memory: { time, kind: 'apply'|'check', dir, calFrames, error }.
  // Empty at start, like a calibration made before the history existed, so both directions read "unchecked".
  const history = [];
  // VST Studio's calibration-inputs.json, in memory (src/Gui/CalEvidence.h): every measurement, combined into the
  // calibration in force.  The demo config counts as calibrated, so it starts as Studio would seed it: one "earlier"
  // input per direction from the frames in force, at Studio's 6% for a calibration of unknown method.
  const linesSig = () => `${config.speedLineLeft},${config.speedLineRight},${config.AnalysisBoxWidth}`;
  const calInputs = DIRS.map((d) => ({
    id: `seed-${d}`, method: 'earlier', label: 'Your earlier calibration', time: localIso(started), dirs: d,
    zoneFt: config[`CalibrationFrames${d}`] * config.CalibrationSpeed * calib.FT_PER_S_PER_MPH / config.CalibrationFPS, sigma: 0.06,
    lines: linesSig(), removed: false,
  }));
  let inputSerial = 0;
  /**
   * Adds a measurement (unless the same one is already counted) and sets CalibrationFrames from the combination, the
   * way App::addCalibrationInput does: a direction's history gets an apply row only when its whole-frame value changes.
   */
  function addInput(input) {
    const dup = calInputs.some((i) => !i.removed && i.method === input.method && i.dirs === input.dirs && i.lines === linesSig()
      && Math.abs(i.zoneFt - input.zoneFt) < 1e-4 * input.zoneFt && Math.abs(i.sigma - input.sigma) < 1e-4);
    if (!dup) calInputs.push({ ...input, id: `in-${++inputSerial}`, time: localIso(now()), lines: linesSig(), removed: false });
    return recombine();
  }
  function recombine() {
    const changes = {};
    for (const d of DIRS) {
      const c = calib.combine(calInputs, d, linesSig());
      if (!c.valid) continue;
      const frames = Math.max(1, Math.round(calib.framesForZone(c.zoneFt, config.CalibrationSpeed, config.CalibrationFPS)));
      if (frames !== config[`CalibrationFrames${d}`]) changes[`CalibrationFrames${d}`] = frames;
    }
    const r = setConfig(changes);
    if (r.ok) for (const k of Object.keys(changes)) history.push({ time: localIso(now()), kind: 'apply', dir: k.slice(-3), calFrames: changes[k], error: null });
    return r;
  }
  const vehicles = [];   // oldest first; ids ascend
  const listeners = new Set();
  let nextId = 1;

  /** Frames a pass at `speed` takes through the zone, from the current calibration (inverse of VST's speed formula). */
  const framesFor = (dir, speed) => {
    const cal = dir === 'L2R' ? config.CalibrationFramesL2R : config.CalibrationFramesR2L;
    return Math.max(1, Math.round(cal * config.CalibrationSpeed / speed));
  };

  /** Builds a vehicle record in the API's shape; `withClip` marks the passes that keep a clip. */
  function makeVehicle(time, dir, speed, withClip) {
    const id = nextId++;
    const frames = framesFor(dir, speed);
    const frame = Math.round((time - started) / 1000 * 30) + 600_000;
    const large = rand() < 0.04;
    const wrongWay = config.legalDirection !== 'both' && dir !== config.legalDirection;
    return {
      id, time: localIso(time), video: 'camera', frame,
      dir, compass: dir === 'L2R' ? config.L2RDirection : config.R2LDirection,
      wrongWay, speed,
      speeding: speed > config.speedLimit, egregious: speed >= config.egregious,
      area: large ? 110 + Math.round(rand() * 60) : 40 + Math.round(rand() * 60), large,
      frames, startFrame: frame - frames - 30, endFrame: frame - 30, fps: 30,
      snapshot: `/api/vehicles/${id}/snapshot.jpg`,
      clip: withClip ? `/api/vehicles/${id}/clip.mp4` : null,
      belowMin: false,   // the demo's passes are all at or over minLoggedSpeed
    };
  }

  /** A plausible pass: mostly legal direction near the limit, ~2% the wrong way (who also tend to be quick). */
  function randomPass(time, withClip) {
    const wrong = rand() < 0.02;
    const dir = wrong ? 'L2R' : 'R2L';
    const speed = Math.round(Math.min(58, Math.max(14, 25.8 + gauss(rand) * 5.2 + (wrong ? 4 : 0) + (rand() < 0.03 ? 12 : 0))));
    return makeVehicle(time, dir, speed, withClip);
  }

  // Two weeks of history, hour by hour, ~170 passes on a weekday and fewer at weekends.
  for (let t = started - 14 * DAY; t < started; t += 3600e3) {
    const d = new Date(t);
    const weekend = d.getDay() === 0 || d.getDay() === 6;
    const n = Math.round(HOURLY[d.getHours()] * (weekend ? 3.6 : 5.6) * (0.7 + rand() * 0.6));
    const times = Array.from({ length: n }, () => t + rand() * 3600e3).filter((x) => x < started).sort((a, b) => a - b);
    for (const at of times) vehicles.push(randomPass(at, started - at < DAY));
  }

  const counts = () => {
    const s = vehicles.filter((v) => Date.parse(v.time) >= started - DAY);
    return {
      vehicles: s.length,
      speeders: s.filter((v) => v.speeding).length,
      egregious: s.filter((v) => v.egregious).length,
      wrongWay: s.filter((v) => v.wrongWay).length,
      topSpeed: s.reduce((m, v) => Math.max(m, v.speed), 0),
      avgSpeed: s.length ? s.reduce((t, v) => t + v.speed, 0) / s.length : 0,
    };
  };
  /** Hand-marked calibration frames as VST Studio reports them: [start, end] per direction, -1 = not marked. */
  const marks = (dir) => [state.marks[dir]?.start ?? -1, state.marks[dir]?.end ?? -1];

  /**
   * One direction's calibration standing, by VST Studio's rule (src/Gui/Calibration.cpp): verified when the latest
   * check after the latest apply is within ±2 mph and was made against the CalibrationFrames still in force.
   * The demo config counts as calibrated, so with no check on record a direction is "unchecked", never "none".
   */
  function calState(dir) {
    let apply = null, check = null;
    for (const e of history) {
      if (e.dir !== dir) continue;
      if (e.kind === 'check') check = e; else { apply = e; check = null; }
    }
    const current = config[`CalibrationFrames${dir}`];
    const checked = check && check.calFrames === current;
    return {
      state: checked ? (Math.abs(check.error) <= 2 ? 'verified' : 'failed') : 'unchecked',
      error: checked ? check.error : null,
      applied: apply ? apply.time : null,
      checked: checked ? check.time : null,
      frames: current,
    };
  }

  /** The pass a calibration request names: a logged vehicle, or the hand-marked span. `{ frames, fps, dir }` or `{ problem }`. */
  function calibrationPass(body) {
    const { dir, speed, vehicleId } = body || {};
    if (!(Number(speed) > 0)) return { problem: 'Pick a direction and the speed you drove' };
    if (vehicleId != null) {
      const v = vehicles.find((x) => x.id === Number(vehicleId));
      if (!v) return { problem: 'That pass is no longer available' };
      return { frames: v.frames, fps: v.fps, dir: DIRS.includes(dir) ? dir : v.dir };
    }
    if (!DIRS.includes(dir)) return { problem: 'Pick a direction and the speed you drove' };
    const mk = state.marks[dir] || {};
    const frames = mk.end - mk.start;
    if (!(frames > 0)) return { problem: 'Mark the start and then the end of the pass first' };
    return { frames, fps: 30, dir };
  }

  function calibrationJson() {
    const out = { inputs: calInputs.map((i) => ({ id: i.id, method: i.method, methodName: calib.METHOD_NAMES[i.method] || 'Measurement',
      label: i.label, time: i.time, dirs: i.dirs, zoneFt: Math.round(i.zoneFt * 100) / 100, sigmaPct: Math.round(i.sigma * 1000) / 10,
      removed: i.removed, otherLines: i.lines !== linesSig() })) };
    for (const d of DIRS) out[d] = { ...calState(d), combined: calib.combinedJson(calInputs, d, linesSig()) };
    return out;
  }

  const status = () => ({
    version: '1.1.0-demo',
    running: state.running, paused: state.paused,
    source: { kind: 'camera', name: 'Demo camera', fps: 30, width: 1280, height: 720 },
    frame: state.frame + Math.round((now() - started) / 1000 * 30),
    totalFrames: 0,
    procFps: state.running && !state.paused ? 29 + Math.round(rand() * 10) / 10 : 0,
    counts: counts(),
    speedLimit: config.speedLimit, egregious: config.egregious,
    legalDirection: config.legalDirection, siteName: config.siteName,
    batch: { index: 0, count: 1 },
    calibrationMarks: { L2R: marks('L2R'), R2L: marks('R2L') },
    calibration: calibrationJson(),
    liveViews: listeners.size,
    health: { camera: 'same', cameraWarning: false, lowDisk: false },
  });

  const emit = (event, data) => { for (const fn of listeners) fn(event, data); };

  /** Re-derives the flags that depend on config so history follows a changed limit or direction. */
  function refreshFlags() {
    for (const v of vehicles) {
      v.compass = v.dir === 'L2R' ? config.L2RDirection : config.R2LDirection;
      v.wrongWay = config.legalDirection !== 'both' && v.dir !== config.legalDirection;
      v.speeding = v.speed > config.speedLimit;
      v.egregious = v.speed >= config.egregious;
    }
  }

  /** Mirrors VST Studio's validation: the same rules VST.cfg loading enforces, in plain words. */
  function validate(c) {
    const p = [];
    const int = (k) => Number.isFinite(c[k]) || p.push(`${k} must be a number`);
    ['AnalysisBoxLeft', 'AnalysisBoxTop', 'AnalysisBoxWidth', 'AnalysisBoxHeight', 'speedLineLeft', 'speedLineRight',
      'R2LStreetY', 'L2RStreetY', 'CalibrationFramesL2R', 'CalibrationFramesR2L', 'CalibrationSpeed', 'CalibrationFPS',
      'speedLimit', 'egregious'].forEach(int);
    if (c.AnalysisBoxLeft < 0 || c.AnalysisBoxLeft + c.AnalysisBoxWidth > 1279) p.push('The analysis box must fit inside the frame horizontally');
    if (c.AnalysisBoxTop < 0 || c.AnalysisBoxTop + c.AnalysisBoxHeight > 719) p.push('The analysis box must fit inside the frame vertically');
    if (c.AnalysisBoxWidth < 200) p.push('The analysis box is too narrow to track anything');
    if (!(c.speedLineLeft >= 0 && c.speedLineLeft < c.speedLineRight && c.speedLineRight <= c.AnalysisBoxWidth)) p.push('Speed lines must sit inside the box, left before right');
    else if (c.speedLineRight - c.speedLineLeft < MIN_ZONE) p.push(`The speed zone must be at least ${MIN_ZONE} px wide`);
    if (!(c.R2LStreetY >= 0 && c.R2LStreetY <= c.AnalysisBoxHeight && c.L2RStreetY >= 0 && c.L2RStreetY <= c.AnalysisBoxHeight)) p.push('Lane lines must sit inside the box');
    if (!['L2R', 'R2L', 'both'].includes(c.legalDirection)) p.push('legalDirection must be L2R, R2L or both');
    if (!(c.speedLimit > 0 && c.egregious > c.speedLimit)) p.push('The "way too fast" speed must be above the limit');
    if (!(c.CalibrationFramesL2R > 0 && c.CalibrationFramesR2L > 0 && c.CalibrationSpeed > 0 && c.CalibrationFPS > 0)) p.push('Calibration values must be positive');
    return p;
  }

  function setConfig(partial) {
    const merged = { ...config, ...partial };
    // Like VST Studio's applyConfig: a new limit moves an automatic minLoggedSpeed, unless the body sets one.
    if (partial.speedLimit != null && partial.minLoggedSpeed == null && merged.speedLimit !== config.speedLimit
      && plan.minLoggedFollows(config.minLoggedSpeed, config.speedLimit)) merged.minLoggedSpeed = plan.minLoggedForLimit(merged.speedLimit);
    const problems = validate(merged);
    if (problems.length) return { ok: false, problems, config: { ...config } };
    Object.assign(config, merged);
    refreshFlags();
    emit('config', { ...config });
    return { ok: true, problems: [], config: { ...config } };
  }

  const ok = (json) => ({ status: 200, json });
  const err = (status, error) => ({ status, json: { error } });

  /**
   * Routes one API request. `path` is the URL path ("/api/…"), `query` a URLSearchParams-like
   * `{ get(name) }`, `body` already-parsed JSON. Returns `{ status, json }` or `{ status, file }`.
   */
  function handle(method, path, query = new URLSearchParams(), body = {}) {
    const m = path.match(/^\/api\/vehicles\/(\d+)\/(snapshot\.jpg|clip\.mp4)$/);
    if (m && method === 'GET') {
      const v = vehicles.find((x) => x.id === Number(m[1]));
      if (!v) return err(404, 'No such vehicle');
      if (m[2] === 'clip.mp4') return v.clip ? { status: 200, file: 'demo/clip.mp4' } : err(404, 'No clip kept for this vehicle');
      return { status: 200, file: `demo/snap${v.id % SNAPSHOTS}.jpg` };
    }
    const one = path.match(/^\/api\/vehicles\/(\d+)$/);
    if (one && method === 'GET') {
      const v = vehicles.find((x) => x.id === Number(one[1]));
      return v ? ok(v) : err(404, 'No such vehicle');
    }
    switch (`${method} ${path}`) {
      case 'GET /api/hello': return ok({ app: 'VST', version: '1.1.0-demo', auth: true });
      case 'GET /api/status': return ok(status());
      case 'GET /api/config': return ok({ ...config });
      case 'PUT /api/config': return ok(setConfig(body || {}));
      case 'GET /api/frame.jpg': return { status: 200, file: 'demo/frame.jpg' };
      case 'GET /api/plan': return ok(planJson());
      case 'PUT /api/plan': {
        if (!body || typeof body !== 'object' || Array.isArray(body)) return err(400, 'body must be a JSON object');
        const problems = setPlan(body);
        return ok({ ok: !problems.length, problems, plan: planJson() });
      }
      case 'GET /api/vehicles': {
        const since = Number(query.get('since') || 0);
        const from = query.get('from') ? Date.parse(query.get('from')) : -Infinity;
        const to = query.get('to') ? Date.parse(query.get('to')) : Infinity;
        const limit = Math.min(5000, Number(query.get('limit') || 100));
        const out = [];
        for (let i = vehicles.length - 1; i >= 0 && out.length < limit; i--) {
          const v = vehicles[i];
          const t = Date.parse(v.time);
          if (v.id > since && t >= from && t <= to) out.push(v);
        }
        return ok(out);
      }
      case 'POST /api/run': {
        const a = body?.action;
        if (a === 'start') Object.assign(state, { running: true, paused: false });
        else if (a === 'stop') Object.assign(state, { running: false, paused: false });
        else if (a === 'pause') state.paused = state.running;
        else if (a === 'resume') state.paused = false;
        else return err(400, 'action must be start, stop, pause or resume');
        emit('status', status());
        return ok({ ok: true, status: status() });
      }
      case 'POST /api/calibration/mark': {
        if (!['L2R', 'R2L'].includes(body?.dir) || !['start', 'end'].includes(body?.edge)) return err(400, 'dir and edge required');
        (state.marks[body.dir] ||= {})[body.edge] = status().frame;
        return ok({ ok: true, frame: state.marks[body.dir][body.edge], marks: marks(body.dir) });
      }
      case 'POST /api/calibration/apply': {
        const pass = calibrationPass(body);
        if (pass.problem) return ok({ ok: false, problems: [pass.problem] });
        const { frames, fps, dir } = pass;
        // Like VST Studio's App::applyCalibration: the pass joins the other measurements (both directions on a one-way
        // street, where it measures the one lane), and CalibrationFrames follows their combination.
        const mph = Number(body.speed);
        return ok(addInput({ method: 'pass', label: `Drive past at ${mph} mph`, dirs: config.legalDirection === 'both' ? dir : 'both',
          zoneFt: calib.zoneFtFromPass(mph, frames, fps), sigma: calib.sigmaPass(mph, frames) }));
      }
      case 'POST /api/calibration/distance': {
        // Like VST Studio: the zone's length from two cones `feet` apart on the picture, then the frames a car at
        // CalibrationSpeed takes over it (src/Gui/Calibration.h framesForDistance), for one direction or both.
        const { cones, feet, dir = 'both' } = body || {};
        const shaped = Array.isArray(cones) && cones.length === 2 && cones.every((c) => Array.isArray(c) && c.length === 2
          && c.every(Number.isFinite) && c[0] >= 0 && c[0] <= 1280 && c[1] >= 0 && c[1] <= 720);
        const markPx = shaped ? Math.abs(cones[1][0] - cones[0][0]) : 0;
        const zonePx = config.speedLineRight - config.speedLineLeft;
        const problem = !shaped ? 'mark both spots on the picture first'
          : !(Number(feet) > 0) ? 'enter the distance between the spots in feet'
          : !['both', 'L2R', 'R2L'].includes(dir) ? 'dir must be both, L2R or R2L'
          : markPx < 40 ? 'the two spots are too close together on the picture; space them further apart' : null;
        if (problem) return ok({ ok: false, problems: [problem], config });
        const zoneFeet = zonePx / markPx * Number(feet);
        const frames = zoneFeet / (config.CalibrationSpeed * 5280 / 3600) * config.CalibrationFPS;
        const r = addInput({ method: 'cones', label: `Cones or pole, ${Number(feet)} ft apart`, dirs: dir, zoneFt: zoneFeet, sigma: calib.sigmaCones(markPx) });
        return ok({ ...r, zoneFeet: Math.round(zoneFeet * 10) / 10, frames: Math.round(frames * 10) / 10 });
      }
      case 'POST /api/calibration/inputs': {
        // Take one measurement out, or put it back, and recombine (Server.cpp's route).
        if (!body || typeof body !== 'object' || Array.isArray(body)) return err(400, 'body must be a JSON object');
        const input = calInputs.find((i) => i.id === body.id);
        if (!body.id) return ok({ ok: false, problems: ['id is required'], calibration: calibrationJson(), config });
        if (!input) return ok({ ok: false, problems: [`no calibration input ${body.id}`], calibration: calibrationJson(), config });
        input.removed = body.removed !== false;
        const r = recombine();
        emit('status', status());
        return ok({ ...r, calibration: calibrationJson() });
      }
      case 'POST /api/calibration/check': {
        const pass = calibrationPass(body);
        if (pass.problem) return ok({ ok: false, problems: [pass.problem] });
        const { frames, fps, dir } = pass;
        const drivenMph = Number(body.speed);
        const calFrames = config[`CalibrationFrames${dir}`];
        // What the calibration in force says the pass was (VST's speed formula, unrounded), and its error, both to
        // 0.1 mph before the ±2 mph verdict so the numbers shown and the verdict agree.
        const measuredMph = Math.round(config.CalibrationSpeed * calFrames * (fps / config.CalibrationFPS) / frames * 10) / 10;
        const error = Math.round((measuredMph - drivenMph) * 10) / 10;
        history.push({ time: localIso(now()), kind: 'check', dir, calFrames, error });
        emit('status', status());
        return ok({ ok: true, problems: [], direction: dir, drivenMph, measuredMph, error, verified: Math.abs(error) <= 2 });
      }
      case 'GET /api/report.html': {
        // A static sample (built by tests/report_test from tests/fixtures/report); VST Studio builds it from the data folder.
        const bad = ['from', 'to'].find((k) => query.get(k) && !/^\d{4}-\d{2}-\d{2}$/.test(query.get(k)));
        return bad ? err(400, 'from and to must be dates like 2026-11-02') : { status: 200, file: 'demo/report.html' };
      }
      case 'GET /api/stats.csv': {
        const rows = vehicles.map((v) => [v.time.slice(0, 10).replace(/-/g, ''), v.time.slice(11, 19).replace(/:/g, ''),
          v.frame, v.compass, v.startFrame, v.endFrame, v.frames, '', '', '', v.area, '', v.speed, '', v.time].join(', '));
        return { status: 200, text: `Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp\n${rows.join('\n')}\n`, type: 'text/csv' };
      }
      default: return err(404, `No route for ${method} ${path}`);
    }
  }

  // Live traffic: a new pass every 12-40 s, preceded by its track crossing the frame, plus a status tick each second.
  let timers = [];
  function scheduleNext() {
    timers.push(setTimeout(() => {
      if (state.running && !state.paused) {
        const v = randomPass(now(), true);
        if (tracks) animateTrack(v, () => { vehicles.push(v); emit('vehicle', v); });
        else { vehicles.push(v); emit('vehicle', v); }
      }
      scheduleNext();
    }, 12e3 + rand() * 28e3));
  }

  /** Emits `tracks` frames moving a box across the analysis box at the pass's speed. */
  function animateTrack(v, done) {
    const c = config;
    const y = c.AnalysisBoxTop + (v.dir === 'R2L' ? c.R2LStreetY : c.L2RStreetY) - 70;
    const x0 = c.AnalysisBoxLeft, x1 = c.AnalysisBoxLeft + c.AnalysisBoxWidth;
    const pxPerSec = (c.speedLineRight - c.speedLineLeft) / (v.frames / 30);
    const w = v.large ? 300 : 190;
    const total = (x1 - x0 + w) / pxPerSec;
    const t0 = now();
    const tick = setInterval(() => {
      const s = (now() - t0) / 1000;
      if (s > total) { clearInterval(tick); emit('tracks', []); done(); return; }
      const travelled = s * pxPerSec;
      const x = v.dir === 'R2L' ? x1 - travelled : x0 - w + travelled;
      const pastZone = v.dir === 'R2L' ? x < c.AnalysisBoxLeft + c.speedLineLeft : x + w > c.AnalysisBoxLeft + c.speedLineRight;
      emit('tracks', [{ id: v.id, dir: v.dir, x: Math.round(x), y: Math.round(y), w, h: 85, speed: pastZone ? v.speed : 0 }]);
    }, 200);
    timers.push(tick);
  }

  /** Starts the simulated feed for one listener; returns an unsubscribe function. Feed runs while anyone listens. */
  function subscribe(fn) {
    listeners.add(fn);
    if (listeners.size === 1) {
      timers.push(setInterval(() => emit('status', status()), 1000));
      scheduleNext();
    }
    fn('status', status());
    fn('config', { ...config });
    return () => {
      listeners.delete(fn);
      if (!listeners.size) { timers.forEach((t) => { clearTimeout(t); clearInterval(t); }); timers = []; }
    };
  }

  return { handle, subscribe };
}
