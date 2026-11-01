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
  R2LStreetY: isInt, L2RStreetY: isInt, R2LStreetYRight: isInt, L2RStreetYRight: isInt,
  streetEdge: (o) => Array.isArray(o) && o.length === 2 && o.every(isInt),
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
  snapshot: (x) => x === null || isStr(x), clip: (c) => c === null || isStr(c), belowMin: isBool,
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
      assert.equal(v.belowMin, false, `vehicle ${v.id}: /api/vehicles without passes=1 lists only logged passes`);
      assert.equal(v.snapshot, `/api/vehicles/${v.id}/snapshot.jpg`);
      if (v.clip !== null) assert.equal(v.clip, `/api/vehicles/${v.id}/clip.mp4`);
    }
  });

  it('passes=1 adds the slow passes (belowMin, no snapshot) to the logged ones, ids descending', async () => {
    const [{ data: logged }, r] = await Promise.all([api.get('/api/vehicles?limit=50'), api.get('/api/vehicles?limit=5000&passes=1')]);
    assert.equal(r.status, 200);
    r.data.forEach((v, i) => assertShape(v, VEHICLE, `passes[${i}]`));
    for (let i = 1; i < r.data.length; i++) assert.ok(r.data[i - 1].id > r.data[i].id, 'ids descend');
    const ids = new Set(r.data.map((v) => v.id));
    for (const v of logged) assert.ok(ids.has(v.id), `logged vehicle ${v.id} is a pass too`);
    for (const v of r.data.filter((x) => x.belowMin)) assert.equal(v.snapshot, null, `slow pass ${v.id} has no snapshot`);
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

  it('a new speedLimit moves an automatic minLoggedSpeed (5 under, at most 18); an explicit one wins', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/config');
    try {
      await api.put('/api/config', { speedLimit: 25, minLoggedSpeed: 18 });
      let r = await api.put('/api/config', { speedLimit: 15 });
      assert.equal(r.data.config.minLoggedSpeed, 10, '15 mph street logs from 10');
      r = await api.put('/api/config', { speedLimit: 25 });
      assert.equal(r.data.config.minLoggedSpeed, 18, 'and back to 18 at 25');
      r = await api.put('/api/config', { speedLimit: 15, minLoggedSpeed: 12 });
      assert.equal(r.data.config.minLoggedSpeed, 12, 'an explicit value in the same body wins');
      r = await api.put('/api/config', { speedLimit: 20 });
      assert.equal(r.data.config.minLoggedSpeed, 12, 'a hand-set value stays');
    } finally {
      await api.put('/api/config', { speedLimit: before.speedLimit, minLoggedSpeed: before.minLoggedSpeed });
    }
  });
});

const PLAN = {
  street: (s) => s && ['distFt', 'clearFt', 'coverFt'].every((k) => isNum(s[k])) && ['guess', 'typed', 'picture', 'calibration', 'map'].includes(s.source)
    && isStr(s.sourceName) && isStr(s.note) && isInt(s.speedLimit),
  presets: (a) => Array.isArray(a) && a.length >= 3 && a.every((x) => isStr(x.key) && isStr(x.name) && isStr(x.hint) && isNum(x.distFt) && isNum(x.clearFt)),
  camera: (c) => c && ['hfov', 'width', 'fps', 'shutter'].every((k) => isNum(c[k])),
  verdict: (v) => v && [0, 1, 2].includes(v.grade) && isStr(v.label) && isStr(v.text) && isInt(v.checkMph) && isNum(v.zoneFt) && isBool(v.shortView)
    && (v.errorMph === null || isNum(v.errorMph)) && (v.fastest === null || isStr(v.fastest)) && (v.frameRate === null || isStr(v.frameRate)),
  measured: (m) => m === null || (isNum(m.coverFt) && isNum(m.clearFt) && isStr(m.how)),
  carLengths: (n) => n === null || isNum(n),
  picture: isBool,
  typicalCarFt: isNum,
  map: (m) => m === null || (['camLat', 'camLon', 'laneLat', 'laneLon', 'distFt'].every((k) => isNum(m[k])) && isStr(m.street) && isStr(m.facing)),
  legalDirection: (d) => ['both', 'L2R', 'R2L'].includes(d),
  L2RDirection: isStr,
  R2LDirection: isStr,
  shortViewOnSave: isBool,
  dirty: isBool,
  changes: (a) => Array.isArray(a) && a.every(isStr),
  autosave: isBool,
  saved: (s) => s && isNum(s.distFt) && isNum(s.clearFt) && isInt(s.speedLimit) && isStr(s.source) && [0, 1, 2].includes(s.grade) && isStr(s.label) && isStr(s.text),
};

describe('GET/PUT /api/plan', () => {
  it('has the documented fields', async () => {
    const r = await api.get('/api/plan');
    assert.equal(r.status, 200);
    assertShape(r.data, PLAN, 'plan');
  });

  it('refuses bad changes with 200 {ok:false, problems, plan} and changes nothing', async () => {
    const { data: before } = await api.get('/api/plan');
    for (const bad of [{ preset: 'castle' }, { distFt: 'far' }, { clearFt: 1 }, { carLengths: 99 }, { autosave: 'yes' }, { map: { camLat: 1 } }, { map: { camLat: 99, camLon: 0, laneLat: 1, laneLon: 0 } }, {}]) {
      const r = await api.put('/api/plan', bad);
      assert.equal(r.status, 200, `${JSON.stringify(bad)}: rejected in-band`);
      assert.equal(r.data.ok, false, `${JSON.stringify(bad)}: ok`);
      assert.ok(r.data.problems.length > 0 && r.data.problems.every(isStr), 'problems in words');
      assert.deepEqual(r.data.plan.street, before.street, '`plan` is the plan still in effect');
    }
    assertError(await api.put('/api/plan', 'not json'), 400, 'bad body');
  });

  it('a preset is a rough guess, typed numbers stick, car lengths size the picture', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes the planner; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/plan');
    try {
      let r = await api.put('/api/plan', { preset: 'narrow' });
      assert.equal(r.data.ok, true);
      assert.equal(r.data.plan.street.source, 'guess');
      assert.equal(r.data.plan.street.distFt, 20);
      r = await api.put('/api/plan', { distFt: 24, clearFt: 30 });
      assert.equal(r.data.plan.street.source, 'typed');
      assert.equal(r.data.plan.street.clearFt, 30);
      r = await api.put('/api/plan', { carLengths: 6 });
      if (r.data.plan.picture) {
        assert.equal(r.data.plan.street.source, 'picture');
        assert.equal(r.data.plan.street.coverFt, 93);
        assert.equal(r.data.plan.carLengths, 6);
      } else {
        // No picture of the user's own street (VST Studio on its bundled sample): refused, saying why.
        assert.equal(r.data.ok, false);
        assert.match(r.data.problems.join(' '), /picture/);
      }
    } finally {
      await api.put('/api/plan', { discard: true });
    }
  });

  it('changes are a preview until saved; discard puts the saved plan back', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes the planner; set MUTATE=1 to run against a real VST Studio');
    let { data: p } = await api.get('/api/plan');
    if (p.autosave) await api.put('/api/plan', { autosave: false });
    await api.put('/api/plan', { discard: true });
    ({ data: p } = await api.get('/api/plan'));
    const saved = p.saved;
    assert.equal(p.dirty, false);
    let r = await api.put('/api/plan', { distFt: 27, clearFt: 31 });
    assert.equal(r.data.plan.dirty, true, 'an edit is unsaved');
    assert.equal(r.data.plan.street.distFt, 27, 'the plan previews the draft');
    assert.deepEqual(r.data.plan.saved, saved, 'the saved plan is untouched');
    assert.ok(r.data.plan.changes.some((c) => /Window to the lane/.test(c)), 'the change is listed in words');
    r = await api.put('/api/plan', { discard: true });
    assert.equal(r.data.plan.dirty, false);
    assert.equal(r.data.plan.street.distFt, saved.distFt);
    // Two pins about 7 m apart, square-on to a north-south one-way: 23 ft, and the legal way read off the map.
    r = await api.put('/api/plan', { map: { camLat: 38.0293, camLon: -78.4900, laneLat: 38.0293, laneLon: -78.48992, wayBearing: 0, oneway: 1, street: 'Test St' } });
    assert.equal(r.data.ok, true, r.data.problems.join('. '));
    assert.equal(r.data.plan.street.source, 'map');
    assert.ok(Math.abs(r.data.plan.street.distFt - 23) < 1, `map distance ${r.data.plan.street.distFt}`);
    assert.equal(r.data.plan.map.facing, 'E');
    assert.equal(r.data.plan.legalDirection, 'R2L', 'facing east, a northbound car crosses right to left');
    await api.put('/api/plan', { discard: true });
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

/**
 * Calibration inputs (src/Gui/CalEvidence.h): each method adds a measurement, and CalibrationFrames is their
 * weighted combination. `added(before)` is the inputs that appeared since a status, `takeOut(list)` removes them again
 * so a test leaves the calibration as it found it, and `framesFor(cfg, zoneFt)` is the engine's frames for a zone.
 */
const inputIds = (st) => new Set((st.calibration.inputs || []).map((i) => i.id));
const added = async (ids) => { const { data: st } = await api.get('/api/status'); return st.calibration.inputs.filter((i) => !ids.has(i.id)); };
const takeOut = async (list) => { for (const i of list) await api.post('/api/calibration/inputs', { id: i.id, removed: true }); };
const framesFor = (cfg, zoneFt) => zoneFt / (cfg.CalibrationSpeed * 5280 / 3600) * cfg.CalibrationFPS;

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

  it('apply from a pass adds it to the calibration; CalibrationFrames<dir> follows the combination, CalibrationSpeed/FPS stay', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: vs } = await api.get('/api/vehicles?limit=1');
    if (!vs.length) return ctx.skip('no logged pass to calibrate from');
    const v = vs[0], key = `CalibrationFrames${v.dir}`;
    const { data: before } = await api.get('/api/config');
    const ids = inputIds((await api.get('/api/status')).data);
    let mine = [];
    try {
      const r = await api.post('/api/calibration/apply', { dir: v.dir, speed: 30, vehicleId: v.id });
      assert.equal(r.status, 200);
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      assert.deepEqual(r.data.problems, []);
      mine = await added(ids);
      assert.equal(mine.length, 1, 'one new input');
      assert.equal(mine[0].method, 'pass');
      assert.ok(Math.abs(mine[0].zoneFt - 30 * 5280 / 3600 * v.frames / v.fps) < 0.01, 'the pass says v × t of street');
      const { data: st } = await api.get('/api/status');
      const comb = st.calibration[v.dir].combined;
      assert.ok(comb.shares.some((x) => x.id === mine[0].id), 'it counts for its direction');
      // The frames in force are the combination's (to the rounding of the reported zone), between what was there and
      // what the pass alone says: it joins the earlier measurements instead of replacing them.
      assert.ok(Math.abs(r.data.config[key] - framesFor(before, comb.zoneFt)) <= 0.5 + 0.02, `${r.data.config[key]} frames vs zone ${comb.zoneFt}`);
      const alone = v.frames * 30 / before.CalibrationSpeed * before.CalibrationFPS / v.fps;
      assert.ok(r.data.config[key] >= Math.min(before[key], alone) - 0.5 && r.data.config[key] <= Math.max(before[key], alone) + 0.5);
      assert.equal(r.data.config.CalibrationSpeed, before.CalibrationSpeed);
      assert.equal(r.data.config.CalibrationFPS, before.CalibrationFPS);
    } finally {
      await takeOut(mine);
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

  it('distance adds the cones to the calibration and keeps CalibrationSpeed/FPS', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/config');
    const ids = inputIds((await api.get('/api/status')).data);
    try {
      // Cones 300 px apart and 10 ft apart: the zone is (speedLineRight - speedLineLeft) / 300 x 10 ft.
      const r = await api.post('/api/calibration/distance', { cones: [[400, 450], [700, 450]], feet: 10, dir: 'L2R' });
      assert.equal(r.status, 200);
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      const zoneFeet = (before.speedLineRight - before.speedLineLeft) / 300 * 10;
      const frames = zoneFeet / (before.CalibrationSpeed * 5280 / 3600) * before.CalibrationFPS;
      assert.ok(Math.abs(r.data.zoneFeet - zoneFeet) <= 0.05 + 1e-9, `zoneFeet ${r.data.zoneFeet} vs ${zoneFeet}`);
      assert.ok(Math.abs(r.data.frames - frames) <= 0.05 + 1e-9, `frames ${r.data.frames} vs ${frames}`);
      const mine = await added(ids);
      assert.equal(mine.length, 1);
      assert.equal(mine[0].method, 'cones');
      assert.equal(mine[0].dirs, 'L2R');
      assert.ok(Math.abs(mine[0].zoneFt - zoneFeet) <= 0.01);
      const { data: st } = await api.get('/api/status');
      const comb = st.calibration.L2R.combined;
      assert.ok(Math.abs(r.data.config.CalibrationFramesL2R - framesFor(before, comb.zoneFt)) <= 0.5 + 0.02);
      assert.equal(r.data.config.CalibrationFramesR2L, before.CalibrationFramesR2L, 'an L2R measurement leaves R2L alone');
      assert.equal(r.data.config.CalibrationSpeed, before.CalibrationSpeed);
      assert.equal(r.data.config.CalibrationFPS, before.CalibrationFPS);
    } finally {
      await takeOut(await added(ids));
      await api.put('/api/config', { CalibrationFramesL2R: before.CalibrationFramesL2R });
    }
  });

  it('a second method adds to the first instead of replacing it, and one can be taken out and put back', async (ctx) => {
    if (!t.mutate) return ctx.skip('changes VST.cfg; set MUTATE=1 to run against a real VST Studio');
    const { data: before } = await api.get('/api/config');
    const ids = inputIds((await api.get('/api/status')).data);
    const zonePx = before.speedLineRight - before.speedLineLeft;
    try {
      // Take out whatever is there, so the two below are the whole calibration.
      await takeOut((await api.get('/api/status')).data.calibration.inputs.filter((i) => !i.removed));
      const a = await api.post('/api/calibration/distance', { cones: [[300, 450], [900, 450]], feet: 20, dir: 'both' });
      assert.equal(a.data.ok, true, JSON.stringify(a.data.problems));
      const b = await api.post('/api/calibration/distance', { cones: [[300, 450], [900, 450]], feet: 21, dir: 'both' });
      assert.equal(b.data.ok, true, JSON.stringify(b.data.problems));
      const mine = await added(ids);
      assert.equal(mine.length, 2, 'both measurements are kept');
      const { data: st } = await api.get('/api/status');
      const comb = st.calibration.L2R.combined;
      // Equal uncertainty (same spots): each has half the say, and the result is their mean, 20.5 ft of spacing.
      assert.equal(comb.n, 2);
      for (const sh of comb.shares) assert.ok(Math.abs(sh.contribution - 0.5) < 0.002);
      assert.ok(Math.abs(comb.zoneFt - zonePx / 600 * 20.5) < 0.02, `zone ${comb.zoneFt}`);
      assert.match(comb.summary, /^2 measurements (agree within|differ by) \d+%$/);
      // Out goes the 21 ft one: the calibration is the 20 ft one alone.  Back in: the mean again.
      const out = await api.post('/api/calibration/inputs', { id: mine[1].id, removed: true });
      assert.equal(out.data.ok, true, JSON.stringify(out.data.problems));
      assert.equal(out.data.calibration.L2R.combined.n, 1);
      assert.equal(out.data.calibration.L2R.combined.summary, '1 measurement so far');
      assert.equal(out.data.config.CalibrationFramesL2R, Math.max(1, Math.round(framesFor(before, zonePx / 600 * 20))));
      assert.ok(out.data.calibration.inputs.find((i) => i.id === mine[1].id).removed, 'kept on record as removed');
      const back = await api.post('/api/calibration/inputs', { id: mine[1].id, removed: false });
      assert.equal(back.data.ok, true);
      assert.equal(back.data.calibration.L2R.combined.n, 2);
    } finally {
      await takeOut(await added(ids));
      for (const i of (await api.get('/api/status')).data.calibration.inputs.filter((x) => ids.has(x.id))) {
        await api.post('/api/calibration/inputs', { id: i.id, removed: false });   // the ones taken out above, back in
      }
      await api.put('/api/config', { CalibrationFramesL2R: before.CalibrationFramesL2R, CalibrationFramesR2L: before.CalibrationFramesR2L });
    }
  });

  it('inputs: an unknown id is {ok:false, problems}; a body that is not JSON is 400', async () => {
    const r = await api.post('/api/calibration/inputs', { id: 'no-such-input', removed: true });
    assert.equal(r.status, 200);
    assert.equal(r.data.ok, false);
    assert.ok(r.data.problems.length > 0 && r.data.problems.every(isStr));
    assertError(await api.post('/api/calibration/inputs', 'not json'), 400, 'inputs with a body that is not JSON');
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
    // What the inputs add up to, per direction, and the inputs themselves (CalEvidence.h).
    assert.ok(Array.isArray(st.calibration.inputs), 'calibration.inputs');
    for (const i of st.calibration.inputs) {
      assertShape(i, { id: isStr, method: isStr, methodName: isStr, label: isStr, time: isStr, dirs: (x) => ['L2R', 'R2L', 'both'].includes(x),
        zoneFt: (x) => isNum(x) && x > 0, sigmaPct: (x) => isNum(x) && x > 0, removed: isBool, otherLines: isBool }, 'calibration input');
    }
    for (const d of DIRS) {
      const k = st.calibration[d].combined;
      assertShape(k, { summary: isStr, detail: isStr, n: isInt, agree: isBool, zoneFt: (x) => x === null || isNum(x),
        sigmaPct: (x) => x === null || isNum(x), setAside: isInt, shares: Array.isArray }, `calibration.${d}.combined`);
      assert.equal(k.shares.length, k.n);
      if (k.n) assert.ok(Math.abs(k.shares.reduce((a, x) => a + x.contribution, 0) - 1) < 0.01, 'shares add up to the whole');
    }
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
    let ids = null;
    try {
      await api.post('/api/calibration/check', { dir: v.dir, speed: Math.round(measured(before, v.dir, v.frames, v.fps)), vehicleId: v.id });
      // A pass far off what the calibration says, so the combination's whole frames change (an input that agrees to
      // the frame leaves the value, and its check, standing).
      ids = inputIds((await api.get('/api/status')).data);
      const r = await api.post('/api/calibration/apply', { dir: v.dir, speed: 60, vehicleId: v.id });
      assert.equal(r.data.ok, true, JSON.stringify(r.data.problems));
      const { data: st } = await api.get('/api/status');
      assertCalState(st.calibration[v.dir], 'after an apply');
      if (r.data.config[key] !== before[key]) {
        assert.equal(st.calibration[v.dir].state, 'unchecked');
        assert.ok(st.calibration[v.dir].applied, 'the apply is on record');
      }
    } finally {
      if (ids) await takeOut(await added(ids));
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
