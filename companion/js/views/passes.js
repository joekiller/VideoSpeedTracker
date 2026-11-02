/**
 * Passes: every logged vehicle, newest first, grouped by day and filterable to speeders or wrong-way
 * drivers; new passes slide in live. `#/passes/<id>` shows one pass from this session with its clip, the facts
 * behind the number, and (when sharing is on) Share buttons; `#/passes/saved/<run>/<frame>` shows one pass from a
 * saved run (no picture is kept for those).
 *
 * The list reads "All saved runs" or "This session" (history.js): the saved scope is what Studio's Results shows
 * after a restart, with each pass wearing its calibration set's band and a chip strip to see one set at a time.
 */
import { html, $, $$, sign, band, arrow, clock, dayLabel, dateLong, icon, passPills, trendOf, BAND_LABEL, toast, sheet } from '../ui.js';
import { passCard, shareCanvas, shareClip, sharingOn } from '../share.js';
import * as history from '../history.js';

let filter = 'all';
const FILTERS = { all: () => true, speeding: (v) => v.speeding, wrong: (v) => v.wrongWay };

export async function render(ctx, id, run, frame) {
  if (id) return detail(ctx, { id: Number(id) });
  if (run) return detail(ctx, { run: decodeURIComponent(run), frame: Number(frame) });
  return list(ctx);
}

/** The pills a saved pass adds: re-timed by a newer calibration, or the user's own calibration drive. */
function savedPills(v) {
  const p = [];
  if (v.calibration) p.push(html`<span class="pill" title="You marked this as your own ${v.calibration === 'check' ? 'check' : 'calibration'} drive; Stats leaves it out">${icon('ruler')}Your ${v.calibration === 'check' ? 'check' : 'drive'}</span>`);
  if (v.adjusted) p.push(html`<span class="pill" title="Measured ${v.measured} mph; re-timed to its calibration set's calibration">Re-timed</span>`);
  return p;
}

async function list({ vst, view }) {
  const st = vst.lastStatus || await vst.status();
  const oneWay = st.legalDirection !== 'both';
  if (!oneWay && filter === 'wrong') filter = 'all';
  await history.loadSets(vst);
  let items = [];
  let limit = 60;   // rendered rows; "Show more" adds pages so a long history stays light on a phone

  view.innerHTML = html`
    <h1>Passes</h1>
    <p class="lede" id="lede"></p>
    ${history.scopeSeg()}
    <div id="sets-host"></div>
    <div class="filters"><div class="seg" id="filter">
      <button data-f="all">All</button><button data-f="speeding">Speeding</button>${oneWay ? html`<button data-f="wrong">Wrong way</button>` : ''}
    </div></div>
    <div id="list"></div>`.__html;

  const thumb = (v) => (v.snapshot
    ? html`<img class="thumb" src="${vst.media(v.snapshot)}" alt="" loading="lazy">`
    : html`<span class="thumb none" title="Pictures are kept only for passes since VST Studio last opened">${icon('camera')}</span>`);
  const row = (v) => html`
    <a class="pass ${history.stripeClass(v.set)}" href="${history.passHref(v)}">
      ${sign(v.speed, band(v))}
      <div class="meta"><b>${v.wrongWay ? `Wrong way · ${v.speed} mph` : `${v.speed} mph`} ${arrow(v.dir)}</b>
        <small>${v.time ? clock(v.time) : 'Time not recorded'} <span class="nowrap">· ${v.compass}-bound</span> ${passPills(v, st.speedLimit)} ${savedPills(v)}</small></div>
      ${thumb(v)}
    </a>`;

  const paint = () => {
    const saved = history.scope() === 'saved';
    $('#lede').textContent = saved ? 'Every vehicle VST has measured and saved, newest first.' : 'Every vehicle VST measured since VST Studio last opened, newest first.';
    $$('#filter button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.f === filter));
    $$('#scope button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.scope === history.scope()));
    $('#sets-host').innerHTML = history.setsStrip().__html ?? '';
    const matching = history.inPicked(items).filter(FILTERS[filter]);
    const shown = matching.slice(0, limit);
    if (!shown.length) {
      const none = filter === 'wrong' ? 'No wrong-way drivers. Nice.' : filter === 'speeding' ? 'Nobody speeding. Nice.'
        : saved ? 'No saved passes yet. Start tracking in VST Studio and they show up here, and stay after it restarts.' : 'No passes since VST Studio opened.';
      const hint = !saved && filter === 'all' && history.savedSupported()
        ? html`<button class="btn" id="to-saved" style="margin-top:12px">${icon('report')}Show all saved runs</button>` : '';
      $('#list').innerHTML = html`<div class="empty-state">${icon('car')}<p>${none}</p>${hint}</div>`.__html;
      $('#to-saved')?.addEventListener('click', () => switchScope('saved'));
      return;
    }
    // Group into days, each with a count of what the filter shows.  Passes whose csv never recorded a time (very old
    // recordings) go under their own heading.
    const days = new Map();
    for (const v of shown) {
      const key = v.time ? new Date(v.time).toDateString() : '';
      (days.get(key) || days.set(key, []).get(key)).push(v);
    }
    $('#list').innerHTML = [...days].map(([day, vs]) => html`
      <div class="day"><h3>${day ? dayLabel(day) : 'Date not recorded'}</h3><span class="small muted">${vs.length} ${vs.length === 1 ? 'pass' : 'passes'}</span></div>
      ${vs.map(row)}`.__html).join('')
      + (matching.length > limit ? html`<button class="btn block" id="more" style="margin-top:16px">Show more</button>`.__html : '');
    $('#more')?.addEventListener('click', () => { limit += 60; paint(); });
  };

  const load = async () => {
    $('#list').innerHTML = html`<div class="empty-state"><p class="muted">Loading passes…</p></div>`.__html;
    try {
      items = await history.passes(vst, { limit: history.scope() === 'saved' ? 5000 : 1000 });
    } catch (e) {
      items = [];
      $('#list').innerHTML = html`<div class="empty-state">${icon('warn')}<p>Couldn't load the passes: ${e.message}</p></div>`.__html;
      return;
    }
    paint();
  };
  const switchScope = (s) => { history.setScope(s); limit = 60; load(); };

  $('#filter').onclick = (e) => { const f = e.target.closest('[data-f]')?.dataset.f; if (f) { filter = f; limit = 60; paint(); } };
  $('#scope')?.addEventListener('click', (e) => { const s = e.target.closest('[data-scope]')?.dataset.scope; if (s && s !== history.scope()) switchScope(s); });
  $('#sets-host').addEventListener('click', (e) => {
    const b = e.target.closest('[data-set]');
    if (b) { history.pick(Number(b.dataset.set)); limit = 60; load(); }
  });
  const onVehicle = (e) => {
    items.unshift(history.scope() === 'saved' ? history.asSaved(e.detail) : e.detail);
    paint();
    if (FILTERS[filter](e.detail)) $('#list .pass')?.classList.add('fresh');
  };
  vst.addEventListener('vehicle', onVehicle);
  await load();
  return () => vst.removeEventListener('vehicle', onVehicle);
}

/**
 * One pass.  `{ id }`: a pass from this session (GET /api/vehicles/<id>), with its picture and clip.  `{ run, frame }`:
 * a pass from a saved run, found in that run's saved passes; no picture is kept for it, and it can't be used to
 * calibrate (the API takes this session's ids), but it can still be shared as a card.
 */
async function detail({ vst, view }, { id, run, frame }) {
  let v;
  const gone = (why) => {
    view.innerHTML = html`<div class="empty-state">${icon('car')}<h2>Pass not found</h2><p>${why}</p><a class="btn" href="#/passes" id="to-saved">${history.savedSupported() ? 'Show all saved runs' : 'Back to passes'}</a></div>`.__html;
    $('#to-saved').onclick = () => { if (history.savedSupported()) history.setScope('saved'); };
  };
  if (id != null) {
    try { v = await vst.vehicle(id); } catch {
      return gone('It may be from before VST Studio restarted. Find it under All saved runs.');
    }
  } else {
    try {
      await history.loadSets(vst);
      v = (await vst.vehicles({ scope: 'saved', run, limit: history.SAVED_LIMIT })).find((x) => x.frame === frame);
    } catch { v = null; }
    if (!v) return gone('Its saved run may have been archived or removed in VST Studio.');
  }
  const st = vst.lastStatus || await vst.status();
  const b = band(v);
  const secs = v.frames / (v.fps || 30);
  const over = v.speed - st.speedLimit;
  const title = v.wrongWay ? 'Wrong-way driver' : over > 0 ? `${over} mph over the limit` : 'Within the limit';
  const clip = vst.media(v.clip), snap = vst.media(v.snapshot);
  const tr = trendOf(v);
  const set = v.set != null ? history.setById(v.set) : null;
  const sw = set ? history.setWords(set) : null;

  view.innerHTML = html`
    <div class="detail">
      <a class="btn ghost" href="#/passes" style="margin:0 0 6px -12px">${icon('back')}Passes</a>
      <div class="media">
        ${clip ? html`<video src="${clip}" poster="${snap}" autoplay muted loop playsinline controls></video>`
          : snap ? html`<img src="${snap}" alt="Snapshot of the vehicle">`
          : html`<div class="no-media">${icon('camera')}<p>No picture for this pass. VST keeps pictures only for passes since VST Studio last opened. Its speed, time and direction stay saved.</p></div>`}
      </div>
      <div class="hero">
        ${sign(v.speed, b, 'MPH', 'lg')}
        <div>
          ${v.wrongWay ? html`<span class="wrongway" style="margin-bottom:8px">WRONG WAY</span>` : ''}
          <h1>${title}</h1>
          <p class="muted" style="margin:4px 0 0">${v.time ? dateLong(v.time) : 'Time not recorded'}</p>
        </div>
      </div>
      <div class="list">
        <div class="item"><div class="label"><b>Speed</b><small>${BAND_LABEL[b]}${v.wrongWay && v.speeding ? ', and speeding' : ''}</small></div><div class="value">${v.speed} mph · limit ${st.speedLimit}</div></div>
        ${v.adjusted ? html`<div class="item"><div class="label"><b>As measured</b><small>Re-timed to ${v.speed} mph by its calibration set's calibration</small></div><div class="value">${v.measured} mph</div></div>` : ''}
        <div class="item"><div class="label"><b>Speed change</b><small>${tr ? `${v.trend.entry} mph at the first line, ${v.trend.exit} at the second` : 'Not seen clearly enough to tell'}</small></div>
          <div class="value">${tr ? html`<span class="pill ${tr.cls}">${icon(tr.icon)}${tr.label}</span>` : '—'}</div></div>
        ${tr ? html`<div class="item"><div class="label"><b>20 ft further on</b><small>At ${v.trend.accel > 0 ? '+' : ''}${v.trend.accel} mph per second${tr.kind === 'steady' ? ', too little to count' : ''}</small></div>
          <div class="value">${v.trend.in20ft === 0 ? 'Stopped' : `About ${v.trend.in20ft} mph`}</div></div>` : ''}
        <div class="item"><div class="label"><b>Direction</b><small>${v.dir === 'L2R' ? 'Left to right' : 'Right to left'} on screen</small></div><div class="value row" style="gap:6px">${v.compass}-bound ${arrow(v.dir)}</div></div>
        <div class="item"><div class="label"><b>Time in speed zone</b><small>How the speed is worked out</small></div><div class="value">${v.frames} frames · ${secs.toFixed(2)} s</div></div>
        <div class="item"><div class="label"><b>Vehicle size</b><small>Relative area on screen</small></div><div class="value">${v.large ? 'Large · ' : ''}${v.area}</div></div>
        ${set ? html`<div class="item ${history.stripeClass(set.id)}"><div class="label"><b>Calibration set</b><small>${sw.line}</small></div><div class="value">Set ${set.id} · ${sw.state}</div></div>` : ''}
        ${v.run ? html`<div class="item"><div class="label"><b>Saved run</b><small>${v.video}</small><small class="file">${v.run}</small></div></div>` : ''}
      </div>
      <h3>Share</h3>
      <div id="share"></div>
      <h3>Calibration</h3>
      ${v.calibration ? html`<div class="callout ok">${icon('check')}<p>This is your own ${v.calibration === 'check' ? 'check' : 'calibration'} drive, so the street's numbers leave it out.</p></div>`
        : v.id != null ? html`<div class="callout">${icon('ruler')}<p>Was this your own car, driven at a known speed? Use it to calibrate VST.
        <br><a class="btn small" href="#/setup/5" id="use-cal" style="margin-top:8px">Calibrate from this pass</a></p></div>`
        : html`<div class="callout">${icon('ruler')}<p>Passes from earlier sessions can't be used to calibrate from the phone. Drive past again (any pass since VST Studio opened works), or mark it in VST Studio's Results.</p></div>`}
    </div>`.__html;

  $('#use-cal')?.addEventListener('click', () => sessionStorage.setItem('vst.calPass', String(v.id)));

  const shareBox = $('#share');
  const paintShare = () => {
    if (!sharingOn()) {
      shareBox.innerHTML = html`<div class="callout">${icon('lock')}<p>Sharing is off, so nothing leaves this phone. Turn it on in
        <a href="#/system">System</a> to share cards and clips.</p></div>`.__html;
      return;
    }
    shareBox.innerHTML = html`<div class="btn-row">
      <button class="btn primary" id="share-card">${icon('share')}Share card</button>
      ${clip ? html`<button class="btn" id="share-clip">${icon('camera')}Share clip</button>` : ''}</div>
      <p class="small muted" style="margin-top:8px">Clips can show plates and people. Check before posting.</p>`.__html;
    $('#share-card').onclick = async () => {
      const c = await passCard(v, st, snap);
      const r = await shareCanvas(c, `pass-${v.id ?? v.frame}`, `${v.speed} mph in a ${st.speedLimit} zone${v.wrongWay ? ', the wrong way' : ''}.`);
      if (r === 'downloaded') toast('Card saved to downloads');
    };
    $('#share-clip')?.addEventListener('click', async () => {
      try {
        const r = await shareClip(clip, `pass-${v.id ?? v.frame}`, `${v.speed} mph in a ${st.speedLimit} zone.`);
        if (r === 'downloaded') toast('Clip saved to downloads');
      } catch (e) { toast(e.message); }
    });
  };
  paintShare();
}
