/**
 * Passes: every logged vehicle, newest first, grouped by day and filterable to speeders or wrong-way
 * drivers; new passes slide in live. `#/passes/<id>` shows one pass with its clip, the facts behind
 * the number, and (when sharing is on) Share buttons.
 */
import { html, $, $$, sign, band, arrow, clock, dayLabel, dateLong, icon, passPills, BAND_LABEL, toast, sheet } from '../ui.js';
import { passCard, shareCanvas, shareClip, sharingOn } from '../share.js';

let filter = 'all';
const FILTERS = { all: () => true, speeding: (v) => v.speeding, wrong: (v) => v.wrongWay };

export async function render(ctx, id) {
  return id ? detail(ctx, Number(id)) : list(ctx);
}

async function list({ vst, view }) {
  const st = vst.lastStatus || await vst.status();
  const oneWay = st.legalDirection !== 'both';
  if (!oneWay && filter === 'wrong') filter = 'all';
  const items = await vst.vehicles({ limit: 1000 });
  let limit = 60;   // rendered rows; "Show more" adds pages so a long history stays light on a phone

  view.innerHTML = html`
    <h1>Passes</h1>
    <p class="lede">Every vehicle VST measured, newest first.</p>
    <div class="filters"><div class="seg" id="filter">
      <button data-f="all">All</button><button data-f="speeding">Speeding</button>${oneWay ? html`<button data-f="wrong">Wrong way</button>` : ''}
    </div></div>
    <div id="list"></div>`.__html;

  const row = (v) => html`
    <a class="pass" href="#/passes/${v.id}">
      ${sign(v.speed, band(v))}
      <div class="meta"><b>${v.wrongWay ? `Wrong way · ${v.speed} mph` : `${v.speed} mph`} ${arrow(v.dir)}</b>
        <small>${clock(v.time)} · ${v.compass}-bound ${passPills(v, st.speedLimit)}</small></div>
      <img class="thumb" src="${vst.media(v.snapshot)}" alt="" loading="lazy">
    </a>`;

  const paint = () => {
    $$('#filter button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.f === filter));
    const matching = items.filter(FILTERS[filter]);
    const shown = matching.slice(0, limit);
    if (!shown.length) {
      $('#list').innerHTML = html`<div class="empty-state">${icon('car')}<p>${filter === 'all' ? 'No passes yet.' : filter === 'wrong' ? 'No wrong-way drivers. Nice.' : 'Nobody speeding. Nice.'}</p></div>`.__html;
      return;
    }
    // Group into days, each with a count of what the filter shows.
    const days = new Map();
    for (const v of shown) {
      const key = new Date(v.time).toDateString();
      (days.get(key) || days.set(key, []).get(key)).push(v);
    }
    $('#list').innerHTML = [...days].map(([day, vs]) => html`
      <div class="day"><h3>${dayLabel(day)}</h3><span class="small muted">${vs.length} ${vs.length === 1 ? 'pass' : 'passes'}</span></div>
      ${vs.map(row)}`.__html).join('')
      + (matching.length > limit ? html`<button class="btn block" id="more" style="margin-top:16px">Show more</button>`.__html : '');
    $('#more')?.addEventListener('click', () => { limit += 60; paint(); });
  };

  $('#filter').onclick = (e) => { const f = e.target.closest('[data-f]')?.dataset.f; if (f) { filter = f; limit = 60; paint(); } };
  const onVehicle = (e) => {
    items.unshift(e.detail);
    paint();
    if (FILTERS[filter](e.detail)) $('#list .pass')?.classList.add('fresh');
  };
  vst.addEventListener('vehicle', onVehicle);
  paint();
  return () => vst.removeEventListener('vehicle', onVehicle);
}

async function detail({ vst, view, go }, id) {
  let v;
  try { v = await vst.vehicle(id); } catch {
    view.innerHTML = html`<div class="empty-state">${icon('car')}<h2>Pass not found</h2><p>It may have been cleared when VST restarted.</p><a class="btn" href="#/passes">Back to passes</a></div>`.__html;
    return;
  }
  const st = vst.lastStatus || await vst.status();
  const b = band(v);
  const secs = v.frames / (v.fps || 30);
  const over = v.speed - st.speedLimit;
  const title = v.wrongWay ? 'Wrong-way driver' : over > 0 ? `${over} mph over the limit` : 'Within the limit';
  const clip = vst.media(v.clip), snap = vst.media(v.snapshot);

  view.innerHTML = html`
    <div class="detail">
      <a class="btn ghost" href="#/passes" style="margin:0 0 6px -12px">${icon('back')}Passes</a>
      <div class="media">
        ${clip ? html`<video src="${clip}" poster="${snap}" autoplay muted loop playsinline controls></video>` : html`<img src="${snap}" alt="Snapshot of the vehicle">`}
      </div>
      <div class="hero">
        ${sign(v.speed, b, 'MPH', 'lg')}
        <div>
          ${v.wrongWay ? html`<span class="wrongway" style="margin-bottom:8px">WRONG WAY</span>` : ''}
          <h1>${title}</h1>
          <p class="muted" style="margin:4px 0 0">${dateLong(v.time)}</p>
        </div>
      </div>
      <div class="list">
        <div class="item"><div class="label"><b>Speed</b><small>${BAND_LABEL[b]}${v.wrongWay && v.speeding ? ', and speeding' : ''}</small></div><div class="value">${v.speed} mph · limit ${st.speedLimit}</div></div>
        <div class="item"><div class="label"><b>Direction</b><small>${v.dir === 'L2R' ? 'Left to right' : 'Right to left'} on screen</small></div><div class="value row" style="gap:6px">${v.compass}-bound ${arrow(v.dir)}</div></div>
        <div class="item"><div class="label"><b>Time in speed zone</b><small>How the speed is worked out</small></div><div class="value">${v.frames} frames · ${secs.toFixed(2)} s</div></div>
        <div class="item"><div class="label"><b>Vehicle size</b><small>Relative area on screen</small></div><div class="value">${v.large ? 'Large · ' : ''}${v.area}</div></div>
      </div>
      <h3>Share</h3>
      <div id="share"></div>
      <h3>Calibration</h3>
      <div class="callout">${icon('ruler')}<p>Was this your own car, driven at a known speed? Use it to calibrate VST.
        <a href="#/setup/4" id="use-cal">Use this pass</a></p></div>
    </div>`.__html;

  $('#use-cal').onclick = () => sessionStorage.setItem('vst.calPass', String(v.id));

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
      const r = await shareCanvas(c, `pass-${v.id}`, `${v.speed} mph in a ${st.speedLimit} zone${v.wrongWay ? ', the wrong way' : ''}.`);
      if (r === 'downloaded') toast('Card saved to downloads');
    };
    $('#share-clip')?.addEventListener('click', async () => {
      try {
        const r = await shareClip(clip, `pass-${v.id}`, `${v.speed} mph in a ${st.speedLimit} zone.`);
        if (r === 'downloaded') toast('Clip saved to downloads');
      } catch (e) { toast(e.message); }
    });
  };
  paintShare();
}
