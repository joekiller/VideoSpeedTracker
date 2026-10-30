/**
 * Live: the camera with zones and tracked vehicles drawn on top, today's counts, and the latest passes.
 *
 * Video is the MJPEG stream in an <img> (no player needed); if the stream fails it falls back to
 * refreshing /api/frame.jpg once a second. Overlay is an SVG in frame coordinates, redrawn on
 * `tracks` and `config` events. Each logged pass pops the speed sign in the corner for a few seconds.
 */
import { html, $, sign, band, arrow, ago, icon, passPills } from '../ui.js';
import { zonesSvg, tracksSvg, W, H } from '../overlay.js';

export async function render({ vst, view, go }) {
  const s = vst.lastStatus || await vst.status();
  const cfg = vst.lastConfig || await vst.config();
  let showZones = true, tracks = [], hideSpeedo = null;
  const src = vst.liveSource();

  view.innerHTML = html`
    <div class="stage" id="stage">
      ${src.kind === 'video'
        ? html`<video id="feed" src="${src.src}" autoplay muted loop playsinline></video>`
        : html`<img id="feed" src="${src.src}" alt="Live camera view">`}
      <svg class="overlay" id="ov" viewBox="0 0 ${W} ${H}" preserveAspectRatio="xMidYMid slice"></svg>
      <div class="hud"><span class="pill rec" id="rec"><i></i>Live</span><span class="pill num" id="fps"></span></div>
      <div class="speedo" id="speedo" hidden></div>
    </div>
    <div class="row between" style="margin-top:12px">
      <div class="seg" style="flex:none"><button id="zones" aria-pressed="true">Zones</button></div>
      <div class="btn-row" id="runctl"></div>
    </div>
    <h3>Today</h3>
    <div class="kpis" id="kpis"></div>
    <div class="row between"><h3>Latest passes</h3><a class="small muted" href="#/passes">See all</a></div>
    <div id="latest"></div>`.__html;

  const ov = $('#ov'), feed = $('#feed');
  /** Frame units per CSS px; the overlay uses `slice`, so the narrower axis decides. */
  const k = () => Math.min(W / (ov.clientWidth || 360), H / (ov.clientHeight || 203));
  const draw = () => {
    const c = vst.lastConfig || cfg, st = vst.lastStatus || s;
    const o = { k: k(), speedLimit: st.speedLimit, egregious: st.egregious };
    ov.innerHTML = (showZones ? zonesSvg(c, o) : '') + tracksSvg(tracks, o);
  };

  // Fallback for browsers or VSTs without MJPEG: poll still frames.
  let stillTimer = null;
  if (feed.tagName === 'IMG') {
    feed.onerror = () => {
      if (stillTimer) return;
      stillTimer = setInterval(() => { feed.src = vst.frameUrl(); }, 1000);
      feed.src = vst.frameUrl();
    };
  }

  const paintStatus = () => {
    const st = vst.lastStatus || s;
    const on = st.running && !st.paused;
    $('#rec').innerHTML = on ? '<i></i>Live' : st.running ? 'Paused' : 'Stopped';
    $('#fps').textContent = on ? `${(st.procFps || 0).toFixed(0)} fps` : '';
    $('#fps').hidden = !on;
    $('#runctl').innerHTML = html`
      ${st.running
        ? html`<button class="btn" data-a="${st.paused ? 'resume' : 'pause'}">${icon(st.paused ? 'play' : 'pause')}${st.paused ? 'Resume' : 'Pause'}</button>`
        : html`<button class="btn primary" data-a="start">${icon('play')}Start tracking</button>`}`.__html;
    const c = st.counts || {};
    const kpi = (n, label, cls = '') => html`<div class="kpi ${cls}"><b>${n ?? 0}</b><span>${label}</span></div>`;
    $('#kpis').innerHTML = html`${kpi(c.vehicles, 'Vehicles')}${kpi(c.speeders, 'Speeding', c.speeders ? 'warn' : '')}
      ${kpi(c.egregious, `${st.egregious}+ mph`, c.egregious ? 'bad' : '')}
      ${st.legalDirection === 'both' ? kpi(c.topSpeed, 'Top mph') : kpi(c.wrongWay, 'Wrong way', c.wrongWay ? 'wrong' : '')}`.__html;
  };

  const passRow = (v) => html`
    <a class="pass" href="#/passes/${v.id}" data-id="${v.id}">
      ${sign(v.speed, band(v))}
      <div class="meta"><b>${v.wrongWay ? 'Wrong way' : `${v.speed} mph`} ${arrow(v.dir)}</b>
        <small>${ago(v.time)} · ${v.compass}-bound ${passPills(v, (vst.lastStatus || s).speedLimit)}</small></div>
      <img class="thumb" src="${vst.media(v.snapshot)}" alt="" loading="lazy">
    </a>`;
  const latest = await vst.vehicles({ limit: 5 });
  const paintLatest = () => {
    $('#latest').innerHTML = latest.length
      ? html`${latest.map(passRow)}`.__html
      : html`<div class="empty-state">${icon('car')}<p>No passes yet. They appear here the moment a vehicle crosses the speed zone.</p></div>`.__html;
  };

  $('#runctl').addEventListener('click', async (e) => {
    const a = e.target.closest('[data-a]')?.dataset.a;
    if (!a) return;
    await vst.run(a);
    await vst.status();
    paintStatus();
  });
  $('#zones').onclick = (e) => { showZones = !showZones; e.currentTarget.setAttribute('aria-pressed', showZones); draw(); };

  const onTracks = (e) => { tracks = e.detail || []; draw(); };
  const onVehicle = (e) => {
    const v = e.detail;
    latest.unshift(v); latest.length = Math.min(latest.length, 5);
    paintLatest();
    $('#latest .pass')?.classList.add('fresh');
    const sp = $('#speedo');
    sp.innerHTML = v.wrongWay ? html`<div class="row" style="flex-direction:column;gap:6px">${sign(v.speed, 'wrong', 'MPH', 'lg')}<span class="wrongway">WRONG WAY</span></div>`.__html : sign(v.speed, band(v), 'MPH', 'lg').__html;
    sp.hidden = false;
    clearTimeout(hideSpeedo);
    hideSpeedo = setTimeout(() => { sp.hidden = true; }, 5000);
  };
  const onResize = () => draw();
  vst.addEventListener('tracks', onTracks);
  vst.addEventListener('vehicle', onVehicle);
  vst.addEventListener('status', paintStatus);
  vst.addEventListener('config', draw);
  window.addEventListener('resize', onResize);

  paintStatus(); paintLatest(); requestAnimationFrame(draw);

  return () => {
    vst.removeEventListener('tracks', onTracks);
    vst.removeEventListener('vehicle', onVehicle);
    vst.removeEventListener('status', paintStatus);
    vst.removeEventListener('config', draw);
    window.removeEventListener('resize', onResize);
    clearInterval(stillTimer); clearTimeout(hideSpeedo);
    feed.removeAttribute('src');   // closes the MJPEG connection
  };
}
