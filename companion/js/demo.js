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
    R2LStreetY: 122, L2RStreetY: 158,
    L2RDirection: 'SW', R2LDirection: 'NE',
    CalibrationFramesL2R: 35, CalibrationFramesR2L: 40, CalibrationSpeed: 25, CalibrationFPS: 30,
    legalDirection: 'R2L',
    speedLimit: 25, egregious: 40, siteName: 'Demo Street',
    // Reported by VST Studio too (docs: "VST Studio also reports"); the companion doesn't edit them.
    obstruction_extent: 30, SENSITIVITY_VALUE: 30, BLUR_SIZE: 20, largeVehicleArea: 100, minLoggedSpeed: 18,
    frameWidth: 1280, frameHeight: 720,
  };
  const state = { running: true, paused: false, frame: 0, marks: {} };
  // VST Studio's calibration-history.csv, in memory: { time, kind: 'apply'|'check', dir, calFrames, error }.
  // Empty at start, like a calibration made before the history existed, so both directions read "unchecked".
  const history = [];
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
    calibration: { L2R: calState('L2R'), R2L: calState('R2L') },
    liveViews: listeners.size,
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
    if (!(c.R2LStreetY >= 0 && c.R2LStreetY <= c.AnalysisBoxHeight && c.L2RStreetY >= 0 && c.L2RStreetY <= c.AnalysisBoxHeight)) p.push('Lane lines must sit inside the box');
    if (!['L2R', 'R2L', 'both'].includes(c.legalDirection)) p.push('legalDirection must be L2R, R2L or both');
    if (!(c.speedLimit > 0 && c.egregious > c.speedLimit)) p.push('The "way too fast" speed must be above the limit');
    if (!(c.CalibrationFramesL2R > 0 && c.CalibrationFramesR2L > 0 && c.CalibrationSpeed > 0 && c.CalibrationFPS > 0)) p.push('Calibration values must be positive');
    return p;
  }

  function setConfig(partial) {
    const merged = { ...config, ...partial };
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
        // Like VST Studio's App::applyCalibration: keep CalibrationSpeed/CalibrationFPS (the other direction is
        // expressed against them) and rescale this direction's frames so the pass reads `speed` mph.
        const cal = Math.max(1, Math.round(frames * Number(body.speed) / config.CalibrationSpeed * config.CalibrationFPS / fps));
        const r = setConfig({ [`CalibrationFrames${dir}`]: cal });
        if (r.ok) history.push({ time: localIso(now()), kind: 'apply', dir, calFrames: cal, error: null });
        return ok(r);
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
