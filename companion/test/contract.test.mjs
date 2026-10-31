/**
 * The companion API contract (docs/companion-api.md), checked over HTTP.
 *
 * Runs against the mock by default and against a real VST Studio with BASE and TOKEN set (see target.mjs), so
 * one suite keeps the mock, the desktop app and the document saying the same thing. Each test names the part of
 * the document it checks.
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { startTarget, client, readEvents } from './target.mjs';

let t, api;
before(async () => { t = await startTarget(); api = client(t); });
after(async () => { await t?.stop(); });

// ---- shapes from the document -------------------------------------------------------------------------

const DIRS = ['L2R', 'R2L'];
const isInt = (v) => Number.isInteger(v);
const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const isStr = (v) => typeof v === 'string';
const isBool = (v) => typeof v === 'boolean';

/** Asserts `obj` has every key in `shape` with a value its check accepts. */
function assertShape(obj, shape, where) {
  assert.equal(typeof obj, 'object', `${where} is an object`);
  assert.ok(obj !== null, `${where} is not null`);
  for (const [key, check] of Object.entries(shape)) {
    assert.ok(key in obj, `${where}.${key} is present`);
    assert.ok(check(obj[key]), `${where}.${key} has the documented type, got ${JSON.stringify(obj[key])}`);
  }
}

const STATUS = {
  version: isStr, running: isBool, paused: isBool,
  source: (s) => s === null || (typeof s === 'object' && ['file', 'camera', 'stream', 'image'].includes(s.kind) && isStr(s.name) && isNum(s.fps) && isInt(s.width) && isInt(s.height)),
  frame: isInt, totalFrames: isInt, procFps: isNum,
  counts: (c) => c && ['vehicles', 'speeders', 'egregious', 'wrongWay', 'topSpeed'].every((k) => isInt(c[k])),
  speedLimit: isInt, egregious: isInt,
  legalDirection: (d) => ['L2R', 'R2L', 'both'].includes(d),
  siteName: isStr,
  health: (h) => h && ['unchecked', 'same', 'moved', 'unclear'].includes(h.camera) && isBool(h.cameraWarning) && isBool(h.lowDisk),
};

const CONFIG = {
  AnalysisBoxLeft: isInt, AnalysisBoxTop: isInt, AnalysisBoxWidth: isInt, AnalysisBoxHeight: isInt,
  speedLineLeft: isInt, speedLineRight: isInt,
  obstruction: (o) => Array.isArray(o) && o.length === 2 && o.every(isInt),
  R2LStreetY: isInt, L2RStreetY: isInt,
  L2RDirection: isStr, R2LDirection: isStr,
  CalibrationFramesL2R: isInt, CalibrationFramesR2L: isInt, CalibrationSpeed: isNum, CalibrationFPS: isNum,
  legalDirection: (d) => ['L2R', 'R2L', 'both'].includes(d),
  speedLimit: isInt, egregious: isInt, siteName: isStr,
  minLoggedSpeed: isInt,
};

const VEHICLE = {
  id: isInt, time: (s) => isStr(s) && !Number.isNaN(Date.parse(s)), video: isStr, frame: isInt,
  dir: (d) => DIRS.includes(d), compass: isStr, wrongWay: isBool,
  speed: isInt, speeding: isBool, egregious: isBool,
  area: isInt, large: isBool, frames: isInt, startFrame: isInt, endFrame: isInt, fps: isNum,
  snapshot: isStr, clip: (c) => c === null || isStr(c),
};

/** "Every error is JSON {"error": "message"}." */
function assertError(r, status, where) {
  assert.equal(r.status, status, `${where}: status`);
  assert.match(r.type, /application\/json/, `${where}: JSON content type`);
  assert.ok(isStr(r.data?.error) && r.data.error.length > 0, `${where}: {error} body, got ${JSON.stringify(r.data)}`);
}

// ---- transport and pairing ----------------------------------------------------------------------------

describe('transport and pairing', () => {
  it('GET /api/hello needs no token and identifies VST', async () => {
    const r = await api.get('/api/hello', { auth: false });
    assert.equal(r.status, 200);
    assertShape(r.data, { app: (a) => a === 'VST', version: isStr, auth: isBool }, 'hello');
  });

  it('a missing or wrong token gets 401 {error}', async () => {
    assertError(await api.get('/api/status', { auth: false }), 401, 'no token');
    assertError(await api.get('/api/status', { token: `wrong-${t.token}` }), 401, 'wrong token');
    assertError(await api.get('/api/config', { auth: false }), 401, 'config without a token');
  });

  it('the token also works as ?token= (for <img>, <video> and EventSource)', async () => {
    const r = await api.get(`/api/status?token=${encodeURIComponent(t.token)}`, { auth: false });
    assert.equal(r.status, 200);
  });

  it('allows CORS', async () => {
    const r = await api.get('/api/hello', { auth: false });
    assert.equal(r.headers.get('access-control-allow-origin'), '*');
    const pre = await api.options('/api/status');
    assert.ok(pre.status === 204 || pre.status === 200, `preflight status ${pre.status}`);
    assert.match(pre.headers.get('access-control-allow-headers') || '', /Authorization/i);
    assert.match(pre.headers.get('access-control-allow-methods') || '', /PUT/);
  });

  it('unknown API routes are 404 {error}', async () => {
    assertError(await api.get('/api/no-such-thing'), 404, 'unknown route');
  });

  it('paths outside the companion folder are 404, never the file', async () => {
    // fetch() resolves "..", so these are what reaches the server after a browser normalises a hostile link.
    for (const p of ['/../../../../etc/passwd', '/%2e%2e/%2e%2e/etc/passwd', '/..%2f..%2f..%2fetc%2fpasswd']) {
      const r = await api.get(p, { auth: false });
      assert.equal(r.status, 404, `${p}: status`);
      assert.doesNotMatch(String(r.data), /root:/, `${p}: no file contents`);
    }
  });
});

// ---- endpoints ----------------------------------------------------------------------------------------

describe('GET /api/status', () => {
  it('has the documented fields', async () => {
    const r = await api.get('/api/status');
    assert.equal(r.status, 200);
    assertShape(r.data, STATUS, 'status');
  });
});

describe('GET /api/config', () => {
  it('has the documented fields, in VST.cfg key names', async () => {
    const r = await api.get('/api/config');
    assert.equal(r.status, 200);
    assertShape(r.data, CONFIG, 'config');
  });
});

describe('GET /api/vehicles', () => {
  it('lists passes newest first, each with the documented fields', async () => {
    const r = await api.get('/api/vehicles?limit=50');
    assert.equal(r.status, 200);
    assert.ok(Array.isArray(r.data));
    assert.ok(r.data.length <= 50, 'limit is honoured');
    r.data.forEach((v, i) => assertShape(v, VEHICLE, `vehicles[${i}]`));
    for (let i = 1; i < r.data.length; i++) assert.ok(r.data[i - 1].id > r.data[i].id, 'ids descend');
  });

  it('flags follow the documented rules', async () => {
    const [{ data: vs }, { data: st }] = await Promise.all([api.get('/api/vehicles?limit=200'), api.get('/api/status')]);
    for (const v of vs) {
      // The flags are fixed when a pass is logged, so they only follow the limits now in force on the mock,
      // which re-derives them; VST Studio keeps what was true at the time.
      if (t.mock) {
        assert.equal(v.speeding, v.speed > st.speedLimit, `vehicle ${v.id} speeding`);
        assert.equal(v.egregious, v.speed >= st.egregious, `vehicle ${v.id} egregious`);
        assert.equal(v.wrongWay, st.legalDirection !== 'both' && v.dir !== st.legalDirection, `vehicle ${v.id} wrongWay`);
      }
      assert.equal(v.snapshot, `/api/vehicles/${v.id}/snapshot.jpg`);
      if (v.clip !== null) assert.equal(v.clip, `/api/vehicles/${v.id}/clip.mp4`);
    }
  });

  it('since= returns only newer ids', async () => {
    const { data: all } = await api.get('/api/vehicles?limit=10');
    if (all.length < 2) return;   // a fresh VST Studio may have nothing logged yet
    const since = all[all.length - 1].id;
    const { data: newer } = await api.get(`/api/vehicles?since=${since}&limit=10`);
    assert.ok(newer.every((v) => v.id > since));
    assert.equal(newer.length, all.length - 1);
  });

  it('GET /api/vehicles/<id> is one pass, 404 {error} when gone', async () => {
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (vs.length) {
      const r = await api.get(`/api/vehicles/${vs[0].id}`);
      assert.equal(r.status, 200);
      assert.deepEqual(r.data, vs[0]);
    }
    assertError(await api.get('/api/vehicles/987654321'), 404, 'missing vehicle');
  });
});

describe('media', () => {
  it('snapshot.jpg is a JPEG, 404 {error} when gone', async () => {
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (vs.length) {
      const r = await api.get(vs[0].snapshot, { raw: true });
      assert.equal(r.status, 200);
      assert.match(r.type, /image\/jpeg/);
      assert.deepEqual([...r.data.subarray(0, 3)], [0xff, 0xd8, 0xff], 'JPEG magic');
    }
    assertError(await api.get('/api/vehicles/987654321/snapshot.jpg'), 404, 'missing snapshot');
  });

  it('clip.mp4 is an MP4, 404 {error} when gone', async () => {
    const { data: vs } = await api.get('/api/vehicles?limit=200');
    const withClip = vs.find((v) => v.clip);
    if (withClip) {
      const r = await api.get(withClip.clip, { raw: true });
      assert.equal(r.status, 200);
      assert.match(r.type, /video\/mp4/);
      assert.equal(r.data.subarray(4, 8).toString('latin1'), 'ftyp', 'MP4 starts with an ftyp box');
    }
    assertError(await api.get('/api/vehicles/987654321/clip.mp4'), 404, 'missing clip');
  });

  it('frame.jpg is a JPEG when there is a picture, else 404 {error}', async () => {
    const r = await api.get('/api/frame.jpg?overlay=0', { raw: true });
    if (r.status === 404) return assertError({ ...r, data: JSON.parse(r.data) }, 404, 'no frame');
    assert.equal(r.status, 200);
    assert.match(r.type, /image\/jpeg/);
    assert.deepEqual([...r.data.subarray(0, 3)], [0xff, 0xd8, 0xff]);
  });

  it('stats.csv is the stats csv, or 404 {error} before any run', async () => {
    const r = await api.get('/api/stats.csv');
    if (r.status === 404) return assertError(r, 404, 'no stats');
    assert.equal(r.status, 200);
    assert.match(r.type, /text\/csv/);
    assert.match(r.data.split('\n')[0], /Date, Time, Frame, Direction/);
  });
});

describe('GET /api/report.html', () => {
  it('is one self-contained HTML page carrying the study label', async () => {
    const r = await api.get('/api/report.html');
    assert.equal(r.status, 200);
    assert.match(r.type, /text\/html/);
    assert.match(r.data, /^<!doctype html>/i);
    assert.match(r.data, /Community traffic study\. Estimated speeds, ±\d+(\.\d)? mph/);
    assert.match(r.data, /@media print/, 'prints cleanly');
    // Nothing loaded from elsewhere: every src/href is a data: URI or an in-page link, and no URL has a scheme.
    for (const [, url] of r.data.matchAll(/(?:src|href)\s*=\s*['"]?([^'"\s>]{0,12})/gi)) assert.match(url, /^(data:|#)/, `external reference ${url}`);
    assert.doesNotMatch(r.data, /https?:\/\//);
    for (const banned of ['radar-accurate', 'enforcement-grade', 'evidence for a ticket']) assert.ok(!r.data.includes(banned), banned);
  });

  it('takes from/to dates; anything else is 400 {error}; it needs the token', async () => {
    assert.equal((await api.get('/api/report.html?from=2016-01-01&to=2030-12-31')).status, 200);
    assertError(await api.get('/api/report.html?from=yesterday'), 400, 'bad from');
    assertError(await api.get('/api/report.html?to=2026-13-45x'), 400, 'bad to');
    assertError(await api.get('/api/report.html', { auth: false }), 401, 'no token');
  });
});

describe('GET /api/live', () => {
  it('sends status and config as soon as it opens', async () => {
    const evs = await readEvents(t, `/api/live?token=${encodeURIComponent(t.token)}`,
      (e) => e.some((x) => x.event === 'status') && e.some((x) => x.event === 'config'));
    const status = evs.find((e) => e.event === 'status');
    const config = evs.find((e) => e.event === 'config');
    assert.ok(status, 'a status event');
    assert.ok(config, 'a config event');
    assertShape(status.data, STATUS, 'status event');
    assertShape(config.data, CONFIG, 'config event');
  });
});

describe('PUT /api/config', () => {
  it('rejects invalid settings with 200 {ok:false, problems, config} and changes nothing', async () => {
    const { data: before } = await api.get('/api/config');
    for (const bad of [
      { speedLineLeft: 900, speedLineRight: 100 },
      { AnalysisBoxLeft: 'left' },
      { AnalysisBoxWidth: 5000 },
    ]) {
      const r = await api.put('/api/config', bad);
      assert.equal(r.status, 200, `${JSON.stringify(bad)}: rejected in-band, not as an HTTP error`);
      assert.equal(r.data.ok, false, `${JSON.stringify(bad)}: ok`);
      assert.ok(Array.isArray(r.data.problems) && r.data.problems.length > 0 && r.data.problems.every(isStr), 'problems in words');
      assert.deepEqual(r.data.config, before, '`config` is the configuration still in effect');
    }
    const { data: after } = await api.get('/api/config');
    assert.deepEqual(after, before);
  });

  it('a body that is not JSON is 400 {error}', async () => {
    assertError(await api.put('/api/config', 'not json'), 400, 'bad body');
  });

  it('applies a valid subset, reports it, and announces it on /api/live', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/config');
    const site = `${before.siteName} (test)`;
    try {
      // Open the live stream first and change the config only once its opening config event has arrived, so
      // the second config event proves "after any config change, from any client" rather than "on connect".
      let put = null;
      const evs = await readEvents(t, `/api/live?token=${encodeURIComponent(t.token)}`, (e) => {
        const configs = e.filter((x) => x.event === 'config');
        if (configs.length && !put) put = api.put('/api/config', { siteName: site });
        return configs.some((x) => x.data.siteName === site);
      }, 8000);
      const r = await put;
      assert.equal(r.status, 200);
      assert.equal(r.data.ok, true);
      assert.deepEqual(r.data.problems, []);
      assert.equal(r.data.config.siteName, site);
      const configs = evs.filter((e) => e.event === 'config');
      assert.equal(configs[0].data.siteName, before.siteName, 'the opening config event is the old one');
      assert.ok(configs.some((e) => e.data.siteName === site), 'a config event announced the change');
      assert.equal((await api.get('/api/config')).data.siteName, site);
      assert.equal((await api.get('/api/status')).data.siteName, site);
    } finally {
      await api.put('/api/config', { siteName: before.siteName });
    }
  });
});

describe('POST /api/run', () => {
  it('an unknown action is 400 {error}', async () => {
    assertError(await api.post('/api/run', { action: 'dance' }), 400, 'unknown action');
  });

  it('answers {ok, status} with the status after the action', async (ctx) => {
    if (!t.mutate) return ctx.skip('starts and stops tracking; set MUTATE=1 to run against a real VST Studio');
    const { data: was } = await api.get('/api/status');
    try {
      const p = await api.post('/api/run', { action: 'pause' });
      assert.equal(p.status, 200);
      assert.equal(p.data.ok, true);
      assertShape(p.data.status, STATUS, 'run reply status');
      if (was.running) assert.equal(p.data.status.paused, true);
      const r = await api.post('/api/run', { action: 'resume' });
      assert.equal(r.data.ok, true);
      assert.equal(r.data.status.paused, false);
    } finally {
      if (was.paused) await api.post('/api/run', { action: 'pause' });
    }
  });
});

describe('calibration', () => {
  it('mark answers {ok, frame}; apply without a full span is {ok:false, problems}', async () => {
    const m = await api.post('/api/calibration/mark', { dir: 'R2L', edge: 'start' });
    assert.equal(m.status, 200);
    assert.equal(m.data.ok, true);
    assert.ok(isInt(m.data.frame));
    // Only the start is marked (or the end is still from an earlier session but not after it): VST refuses.
    const a = await api.post('/api/calibration/apply', { dir: 'R2L', speed: 0 });
    assert.equal(a.status, 200);
    assert.equal(a.data.ok, false);
    assert.ok(a.data.problems.length > 0 && a.data.problems.every(isStr));
  });

  it('apply from a pass rescales CalibrationFrames<dir> and keeps CalibrationSpeed/FPS', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (!vs.length) return ctx.skip('no logged pass to calibrate from');
    const v = vs[0], key = `CalibrationFrames${v.dir}`;
    const { data: before } = await api.get('/api/config');
    try {
      const r = await api.post('/api/calibration/apply', { dir: v.dir, speed: 30, vehicleId: v.id });
      assert.equal(r.status, 200);
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      assert.deepEqual(r.data.problems, []);
      const want = Math.max(1, Math.round(v.frames * 30 / before.CalibrationSpeed * before.CalibrationFPS / v.fps));
      assert.equal(r.data.config[key], want);
      assert.equal(r.data.config.CalibrationSpeed, before.CalibrationSpeed);
      assert.equal(r.data.config.CalibrationFPS, before.CalibrationFPS);
    } finally {
      await api.put('/api/config', { [key]: before[key] });
    }
  });

  it('distance: cones missing, too close or no distance is {ok:false, problems}', async () => {
    for (const bad of [{}, { cones: [[300, 400]], feet: 10 }, { cones: [[300, 400], [320, 400]], feet: 10 }, { cones: [[300, 400], [900, 400]] },
      { cones: [[300, 400], [900, 400]], feet: 10, dir: 'up' }]) {
      const r = await api.post('/api/calibration/distance', bad);
      assert.equal(r.status, 200, JSON.stringify(bad));
      assert.equal(r.data.ok, false, JSON.stringify(bad));
      assert.ok(r.data.problems.length > 0 && r.data.problems.every(isStr));
    }
    assertError(await api.post('/api/calibration/distance', 'not json'), 400, 'distance with a body that is not JSON');
  });

  it('distance sets CalibrationFrames from the cones and keeps CalibrationSpeed/FPS', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/config');
    try {
      // Cones 300 px apart and 10 ft apart: the zone is (speedLineRight - speedLineLeft) / 300 x 10 ft.
      const r = await api.post('/api/calibration/distance', { cones: [[400, 450], [700, 450]], feet: 10, dir: 'L2R' });
      assert.equal(r.status, 200);
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      const zoneFeet = (before.speedLineRight - before.speedLineLeft) / 300 * 10;
      const frames = zoneFeet / (before.CalibrationSpeed * 5280 / 3600) * before.CalibrationFPS;
      assert.ok(Math.abs(r.data.zoneFeet - zoneFeet) <= 0.05 + 1e-9, `zoneFeet ${r.data.zoneFeet} vs ${zoneFeet}`);
      assert.ok(Math.abs(r.data.frames - frames) <= 0.05 + 1e-9, `frames ${r.data.frames} vs ${frames}`);
      assert.equal(r.data.config.CalibrationFramesL2R, Math.max(1, Math.round(frames)));
      assert.equal(r.data.config.CalibrationFramesR2L, before.CalibrationFramesR2L);
      assert.equal(r.data.config.CalibrationSpeed, before.CalibrationSpeed);
      assert.equal(r.data.config.CalibrationFPS, before.CalibrationFPS);
    } finally {
      await api.put('/api/config', { CalibrationFramesL2R: before.CalibrationFramesL2R });
    }
  });

  it('apply from a vehicle that is gone is {ok:false, problems}', async () => {
    const a = await api.post('/api/calibration/apply', { dir: 'L2R', speed: 25, vehicleId: 987654321 });
    assert.equal(a.status, 200);
    assert.equal(a.data.ok, false);
    assert.ok(a.data.problems.length > 0);
  });
});

// ---- calibration checks (v1.2) ------------------------------------------------------------------------

const CAL_STATES = ['none', 'unchecked', 'verified', 'failed'];
const CAL_DIR = {
  state: (s) => CAL_STATES.includes(s),
  error: (e) => e === null || isNum(e),
  applied: (s) => s === null || (isStr(s) && !Number.isNaN(Date.parse(s))),
  checked: (s) => s === null || (isStr(s) && !Number.isNaN(Date.parse(s))),
  frames: isInt,
};

/** "error is null unless state is verified or failed, and verified means |error| <= 2". */
function assertCalState(c, where) {
  assertShape(c, CAL_DIR, where);
  const checked = c.state === 'verified' || c.state === 'failed';
  assert.equal(c.error !== null, checked, `${where}: error is set exactly when a check decides the state`);
  assert.equal(c.checked !== null, checked, `${where}: checked is set exactly when a check decides the state`);
  if (checked) assert.equal(c.state === 'verified', Math.abs(c.error) <= 2, `${where}: verified means within ±2 mph`);
}

/** What the calibration in `cfg` says a pass was, to 0.1 mph (the document's formula). */
const measured = (cfg, dir, frames, fps) =>
  Math.round(cfg.CalibrationSpeed * cfg[`CalibrationFrames${dir}`] * (fps / cfg.CalibrationFPS) / frames * 10) / 10;

describe('calibration state and checks', () => {
  it('GET /api/status has calibration per direction', async () => {
    const { data: st } = await api.get('/api/status');
    assert.ok(st.calibration && typeof st.calibration === 'object', 'status.calibration');
    for (const d of DIRS) assertCalState(st.calibration[d], `status.calibration.${d}`);
    const { data: cfg } = await api.get('/api/config');
    for (const d of DIRS) assert.equal(st.calibration[d].frames, cfg[`CalibrationFrames${d}`], `${d} frames match the config`);
  });

  it('check refuses what it cannot measure with 200 {ok:false, problems}', async () => {
    for (const bad of [{ dir: 'L2R', speed: 0, vehicleId: 1 }, { dir: 'L2R', speed: 25, vehicleId: 987654321 }]) {
      const r = await api.post('/api/calibration/check', bad);
      assert.equal(r.status, 200, `${JSON.stringify(bad)}: refused in-band`);
      assert.equal(r.data.ok, false);
      assert.ok(Array.isArray(r.data.problems) && r.data.problems.length > 0 && r.data.problems.every(isStr));
    }
    assertError(await api.post('/api/calibration/check', 'not json'), 400, 'check with a body that is not JSON');
  });

  it('check reports measured vs driven, changes no settings, and sets the state', async (ctx) => {
    if (!t.mutate) return ctx.skip('adds rows to calibration-history.csv; set MUTATE=1 to run against a real VST Studio');
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (!vs.length) return ctx.skip('no logged pass to check with');
    const v = vs[0];
    const { data: before } = await api.get('/api/config');
    const want = measured(before, v.dir, v.frames, v.fps);

    // A pass 10 mph slower than the calibration says: off by about +10, not verified.
    const far = Math.max(1, Math.round(want) - 10);
    const bad = await api.post('/api/calibration/check', { dir: v.dir, speed: far, vehicleId: v.id });
    assert.equal(bad.status, 200);
    assertShape(bad.data, { ok: (x) => x === true, direction: (d) => d === v.dir, drivenMph: isNum, measuredMph: isNum, error: isNum, verified: isBool }, 'check reply');
    assert.ok(Math.abs(bad.data.measuredMph - want) < 0.051, `measuredMph ${bad.data.measuredMph}, expected ${want}`);
    assert.equal(bad.data.drivenMph, far);
    assert.ok(Math.abs(bad.data.error - (bad.data.measuredMph - far)) < 0.051, 'error = measuredMph - drivenMph');
    assert.equal(bad.data.verified, false);
    let { data: st } = await api.get('/api/status');
    assert.equal(st.calibration[v.dir].state, 'failed');
    assert.ok(Math.abs(st.calibration[v.dir].error - bad.data.error) < 0.051);

    // The same pass at the speed the calibration says it was: verified, and the state follows.
    const near = Math.round(want);
    const good = await api.post('/api/calibration/check', { dir: v.dir, speed: near, vehicleId: v.id });
    assert.equal(good.data.ok, true);
    assert.equal(good.data.verified, true);
    assert.ok(Math.abs(good.data.error) <= 0.5);
    ({ data: st } = await api.get('/api/status'));
    assertCalState(st.calibration[v.dir], 'after a good check');
    assert.equal(st.calibration[v.dir].state, 'verified');

    const { data: after } = await api.get('/api/config');
    assert.deepEqual(after, before, 'a check changes no settings');
  });

  it('a new apply makes the direction "unchecked" again', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (!vs.length) return ctx.skip('no logged pass to calibrate from');
    const v = vs[0], key = `CalibrationFrames${v.dir}`;
    const { data: before } = await api.get('/api/config');
    try {
      await api.post('/api/calibration/check', { dir: v.dir, speed: Math.round(measured(before, v.dir, v.frames, v.fps)), vehicleId: v.id });
      const r = await api.post('/api/calibration/apply', { dir: v.dir, speed: 30, vehicleId: v.id });
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      const { data: st } = await api.get('/api/status');
      assertCalState(st.calibration[v.dir], 'after an apply');
      assert.equal(st.calibration[v.dir].state, 'unchecked');
      assert.ok(st.calibration[v.dir].applied, 'the apply is on record');
    } finally {
      await api.put('/api/config', { [key]: before[key] });
    }
  });

  it('check from hand-marked frames uses the marked span', async (ctx) => {
    // VST Studio marks the frame on screen, which only moves while video plays; the mock's clock always runs.
    if (!t.mock) return ctx.skip('needs a moving frame counter (the mock)');
    const s = await api.post('/api/calibration/mark', { dir: 'L2R', edge: 'start' });
    await new Promise((res) => setTimeout(res, 250));
    const e = await api.post('/api/calibration/mark', { dir: 'L2R', edge: 'end' });
    const frames = e.data.frame - s.data.frame;
    assert.ok(frames > 0, 'the clock moved between the marks');
    const { data: cfg } = await api.get('/api/config');
    const r = await api.post('/api/calibration/check', { dir: 'L2R', speed: 25 });
    assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
    assert.equal(r.data.direction, 'L2R');
    assert.ok(Math.abs(r.data.measuredMph - measured(cfg, 'L2R', frames, 30)) < 0.051);
  });
});
