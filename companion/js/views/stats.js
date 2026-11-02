/**
 * Stats: the numbers for the "how fast is traffic on my street" argument, computed on the phone from
 * /api/vehicles for the chosen period, over all saved runs or this session (history.js), optionally one
 * calibration set. The user's own calibration and check drives are left out, as on the desktop: they are
 * the user driving at a speed they picked, not the street's traffic.
 *
 * Headline is the 85th percentile speed, the figure traffic engineers use; around it the share over
 * the limit, way-too-fast and wrong-way counts, a speed histogram against the limit, and passes by
 * hour of day (stacked: within limit / over). Every chart has a hover/tap tooltip and a table view.
 */
import { html, $, $$, icon, toast } from '../ui.js';
import { reportCard, shareCanvas, sharingOn } from '../share.js';
import * as history from '../history.js';

// [label, days]; `all` is every pass in the scope (the session, or everything saved).
const PERIODS = { day: ['24 hours', 1], week: ['7 days', 7], month: ['30 days', 30], all: ['All', Infinity] };
let period = 'week';

/** Nearest-rank percentile of a sorted array. */
export const pct = (sorted, p) => (sorted.length ? sorted[Math.min(sorted.length - 1, Math.ceil(p / 100 * sorted.length) - 1)] : 0);

export function summarize(vs, limit, egregiousAt, oneWay) {
  const speeds = vs.map((v) => v.speed).sort((a, b) => a - b);
  const n = speeds.length;
  const lo = Math.min(15, speeds[0] ?? 15), hi = Math.max(limit + 20, speeds[n - 1] ?? 0);
  const histogram = [];
  for (let s = lo; s <= hi; s++) histogram.push({ speed: s, n: 0 });
  for (const s of speeds) histogram[s - lo].n++;
  const hours = Array.from({ length: 24 }, (_, h) => ({ h, ok: 0, over: 0 }));
  for (const v of vs) hours[new Date(v.time).getHours()][v.speed > limit ? 'over' : 'ok']++;
  return {
    n, p85: pct(speeds, 85), median: pct(speeds, 50), top: speeds[n - 1] ?? 0,
    pctSpeeding: n ? Math.round(vs.filter((v) => v.speed > limit).length / n * 100) : 0,
    egregious: vs.filter((v) => v.speed >= egregiousAt).length, egregiousAt,
    wrong: vs.filter((v) => v.wrongWay).length, oneWay, histogram, hours,
  };
}

export async function render({ vst, view }) {
  const st = vst.lastStatus || await vst.status();
  await history.loadSets(vst);
  view.innerHTML = html`
    <h1>Stats</h1>
    <p class="lede" id="lede"></p>
    ${history.scopeSeg()}
    <div class="seg" id="period" style="margin-top:8px">${Object.entries(PERIODS).map(([k, [l]]) => html`<button data-p="${k}">${l}</button>`)}</div>
    <div id="sets-host" style="margin-top:12px"></div>
    <div id="body" style="margin-top:12px"></div>`.__html;

  let note = '';   // what the numbers are made of: sets, re-timed passes, a cut-off
  const load = async () => {
    const saved = history.scope() === 'saved';
    $('#lede').textContent = saved ? 'How fast traffic really goes, from every vehicle VST has measured and saved.' : 'How fast traffic really goes, from every vehicle VST measured since VST Studio last opened.';
    $$('#period button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.p === period));
    $$('#scope button').forEach((b) => b.setAttribute('aria-pressed', b.dataset.scope === history.scope()));
    $('#sets-host').innerHTML = history.setsStrip().__html ?? '';
    const days = PERIODS[period][1];
    const limit = saved ? history.SAVED_LIMIT : 5000;
    let vs;
    try {
      vs = await history.passes(vst, { from: Number.isFinite(days) ? history.localStamp(Date.now() - days * 86400e3) : null, limit });
    } catch (e) {
      $('#body').innerHTML = html`<div class="empty-state">${icon('warn')}<p>Couldn't load the passes: ${e.message}</p></div>`.__html;
      return;
    }
    const own = vs.filter((v) => v.calibration).length;
    const street = vs.filter((v) => !v.calibration);
    const sets = new Set(street.map((v) => v.set).filter((x) => x != null));
    const retimed = street.filter((v) => v.adjusted).length;
    const parts = [];
    if (vs.length >= limit) parts.push(`Based on the newest ${limit.toLocaleString()} passes; the Street report covers every one.`);
    if (sets.size > 1) parts.push(`From ${sets.size} calibration sets, each shown at its own calibration.`);
    if (retimed) parts.push(`${retimed.toLocaleString()} ${retimed === 1 ? 'pass was' : 'passes were'} re-timed to their calibration set's calibration.`);
    if (own) parts.push(`Your own ${own === 1 ? 'calibration drive is' : `${own} calibration drives are`} left out.`);
    note = parts.join(' ');
    paint(summarize(street, st.speedLimit, st.egregious, st.legalDirection !== 'both'));
  };

  const paint = (s) => {
    if (!s.n) {
      const saved = history.scope() === 'saved';
      const when = period === 'all' ? (saved ? 'saved yet' : 'since VST Studio opened') : `in the last ${PERIODS[period][0]}`;
      const hint = !saved && history.savedSupported() ? html`<button class="btn" id="to-saved" style="margin-top:12px">Show all saved runs</button>`
        : period !== 'all' ? html`<button class="btn" id="to-all" style="margin-top:12px">Show all</button>` : '';
      $('#body').innerHTML = html`<div class="empty-state">${icon('gauge')}<p>No passes ${when}.</p>${hint}</div>`.__html;
      $('#to-saved')?.addEventListener('click', () => { history.setScope('saved'); load(); });
      $('#to-all')?.addEventListener('click', () => { period = 'all'; load(); });
      return;
    }
    const verdict = s.p85 <= st.speedLimit ? 'Most drivers keep to the limit.' : `Typical fast drivers go ${s.p85 - st.speedLimit} mph over the limit.`;
    $('#body').innerHTML = html`
      <div class="card hero-stat">
        <div class="grow">
          <div class="step-kicker">85th percentile speed</div>
          <div class="big">${s.p85}<small>mph</small></div>
          <p style="margin:8px 0 0">${verdict} 85% of drivers go this fast or slower; traffic engineers use this number when they review a street's limit.</p>
        </div>
      </div>
      ${note ? html`<p class="small muted" style="margin:10px 2px 0">${note}</p>` : ''}
      <div class="kpis">
        <div class="kpi warn"><b>${s.pctSpeeding}%</b><span>Over ${st.speedLimit}</span></div>
        <div class="kpi"><b>${s.n}</b><span>Vehicles</span></div>
        <div class="kpi ${s.egregious ? 'bad' : ''}"><b>${s.egregious}</b><span>${s.egregiousAt}+ mph</span></div>
        ${s.oneWay ? html`<div class="kpi ${s.wrong ? 'wrong' : ''}"><b>${s.wrong}</b><span>Wrong way</span></div>` : html`<div class="kpi"><b>${s.top}</b><span>Top mph</span></div>`}
      </div>
      <h3>Speeds</h3>
      <div class="card"><div class="chart" id="hist"></div>
        <div class="legend"><span><i class="sw" style="background:var(--bar-neutral)"></i>At or under ${st.speedLimit}</span>
        <span><i class="sw" style="background:var(--bar-warn)"></i>Speeding</span>
        <span><i class="sw" style="background:var(--bad)"></i>${s.egregiousAt}+ mph</span></div>
        <details class="table"><summary>Show as table</summary><table><tr><th>mph</th><th>vehicles</th></tr>
          ${s.histogram.filter((b) => b.n).map((b) => html`<tr><td>${b.speed}</td><td>${b.n}</td></tr>`)}</table></details>
      </div>
      <h3>Time of day</h3>
      <div class="card"><div class="chart" id="hours"></div>
        <div class="legend"><span><i class="sw" style="background:var(--bar-neutral)"></i>Within limit</span>
        <span><i class="sw" style="background:var(--bar-warn)"></i>Over limit</span></div>
        <details class="table"><summary>Show as table</summary><table><tr><th>hour</th><th>within</th><th>over</th></tr>
          ${s.hours.map((h) => html`<tr><td>${hourLabel(h.h)}</td><td>${h.ok}</td><td>${h.over}</td></tr>`)}</table></details>
      </div>
      <h3>Take it to the meeting</h3>
      <div class="btn-row" id="actions">
        <a class="btn primary" id="street-report" href="${vst.reportUrl(reportRange(period))}" target="_blank" rel="noopener">${icon('report')}Street report</a>
        ${sharingOn() ? html`<button class="btn" id="report">${icon('share')}Share report card</button>` : ''}
        ${vst.csvUrl() ? html`<a class="btn" href="${vst.csvUrl()}" download="vst-stats.csv">${icon('download')}${history.scope() === 'saved' ? 'Latest run CSV' : 'Download CSV'}</a>` : ''}
      </div>
      ${sharingOn() ? '' : html`<p class="small muted" style="margin-top:8px">Turn on sharing in <a href="#/system">System</a> to make a report card image.</p>`}`.__html;

    histogram($('#hist'), s, st.speedLimit);
    hourly($('#hours'), s);
    $('#report')?.addEventListener('click', async () => {
      const c = reportCard(s, { speedLimit: st.speedLimit, siteName: st.siteName, period: period === 'all' ? (history.scope() === 'saved' ? 'all saved data' : 'this session') : `last ${PERIODS[period][0]}` });
      if ((await shareCanvas(c, 'traffic-report', `85% of drivers on our street go ${s.p85} mph or slower; ${s.pctSpeeding}% exceed the ${st.speedLimit} mph limit.`)) === 'downloaded') toast('Report card saved to downloads');
    });
  };

  $('#period').onclick = (e) => { const p = e.target.closest('[data-p]')?.dataset.p; if (p) { period = p; load(); } };
  $('#scope')?.addEventListener('click', (e) => { const sc = e.target.closest('[data-scope]')?.dataset.scope; if (sc && sc !== history.scope()) { history.setScope(sc); load(); } });
  $('#sets-host').addEventListener('click', (e) => { const b = e.target.closest('[data-set]'); if (b) { history.pick(Number(b.dataset.set)); load(); } });
  await load();
}

/**
 * Local dates covering the chosen period, for the street report (VST Studio builds a printable study of those days
 * from its data folder: coverage, speeds per direction, calibration record, error band).
 */
export function reportRange(p, now = new Date()) {
  if (!Number.isFinite(PERIODS[p][1])) return {};   // all: from the first data to the last
  const day = (d) => `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, '0')}-${String(d.getDate()).padStart(2, '0')}`;
  return { from: day(new Date(now.getTime() - PERIODS[p][1] * 86400e3)), to: day(now) };
}

const hourLabel = (h) => (h === 0 ? '12a' : h < 12 ? `${h}a` : h === 12 ? '12p' : `${h - 12}p`);

/**
 * Bar chart scaffold: draws bars from `cols` ([{x label, segments:[{v, cls}], tip}]) into `el` with a
 * recessive grid, rounded data ends, and a tooltip that follows the hovered/tapped column.
 */
function bars(el, cols, { marker, labelEvery = 1 } = {}) {
  const Wd = 380, Ht = 190, pl = 28, pb = 20, pt = 16;   // close to on-screen px, so 11px labels stay 11px
  const max = Math.max(1, ...cols.map((c) => c.segments.reduce((a, s) => a + s.v, 0)));
  const nice = niceMax(max);
  const bw = (Wd - pl) / cols.length;
  const y = (v) => Ht - pb - (v / nice) * (Ht - pb - pt);
  let svg = '';
  for (let i = 0; i <= 4; i++) {
    const v = (nice / 4) * i;
    svg += `<line class="grid" x1="${pl}" x2="${Wd}" y1="${y(v)}" y2="${y(v)}"/><text x="${pl - 6}" y="${y(v) + 4}" text-anchor="end">${Math.round(v)}</text>`;
  }
  cols.forEach((c, i) => {
    let base = 0;
    const x = pl + i * bw + Math.min(2, bw * 0.15);
    const w = Math.max(1, bw - Math.min(4, bw * 0.3));
    c.segments.forEach((s, j) => {
      if (!s.v) return;
      const top = j === c.segments.length - 1 || !c.segments.slice(j + 1).some((t) => t.v);
      const y0 = y(base), y1 = y(base + s.v) + (base ? 1 : 0);   // 1px gap between stacked segments
      const r = top ? Math.min(4, w / 2) : 0;
      svg += `<path class="bar ${s.cls}" d="M${x} ${y0}V${y1 + r}${r ? `Q${x} ${y1} ${x + r} ${y1}H${x + w - r}Q${x + w} ${y1} ${x + w} ${y1 + r}` : `H${x + w}`}V${y0}Z"/>`;
      base += s.v;
    });
    if (i % labelEvery === 0) svg += `<text x="${x + w / 2}" y="${Ht - 6}" text-anchor="middle">${c.x}</text>`;
    svg += `<rect class="hit" data-i="${i}" x="${pl + i * bw}" y="${pt}" width="${bw}" height="${Ht - pb - pt}" fill="transparent"/>`;
  });
  if (marker) {
    const mx = pl + marker.at * bw;
    svg += `<line class="limit" x1="${mx}" x2="${mx}" y1="${pt - 6}" y2="${Ht - pb}"/><text class="limit-label" x="${mx + 4}" y="${pt}">${marker.label}</text>`;
  }
  el.innerHTML = `<svg viewBox="0 0 ${Wd} ${Ht}" role="img">${svg}</svg><div class="tip"></div>`;
  const tip = el.querySelector('.tip');
  const show = (e) => {
    const r = e.target.closest('.hit');
    if (!r) { tip.classList.remove('on'); return; }
    const c = cols[r.dataset.i];
    const box = el.getBoundingClientRect(), rb = r.getBoundingClientRect();
    tip.textContent = c.tip;
    tip.style.left = `${Math.min(box.width - 60, Math.max(60, rb.left - box.left + rb.width / 2))}px`;
    tip.style.top = `${rb.top - box.top + 8}px`;
    tip.classList.add('on');
  };
  el.addEventListener('pointermove', show);
  el.addEventListener('pointerdown', show);
  el.addEventListener('pointerleave', () => tip.classList.remove('on'));
}

function niceMax(v) {
  const p = 10 ** Math.floor(Math.log10(v));
  return [1, 2, 2.5, 5, 10].map((m) => m * p).find((m) => m >= v);
}

function histogram(el, s, limit) {
  const lo = s.histogram[0].speed;
  bars(el, s.histogram.map((b) => ({
    x: b.speed % 5 === 0 ? b.speed : '',
    segments: [{ v: b.n, cls: b.speed >= s.egregiousAt ? 'fill-bad' : b.speed > limit ? 'fill-warn' : 'fill-ok' }],
    tip: `${b.speed} mph · ${b.n} ${b.n === 1 ? 'vehicle' : 'vehicles'}`,
  })), { marker: { at: limit - lo + 1, label: `LIMIT ${limit}` } });
}

function hourly(el, s) {
  bars(el, s.hours.map((h) => ({
    x: h.h % 3 === 0 ? hourLabel(h.h) : '',
    segments: [{ v: h.ok, cls: 'fill-ok' }, { v: h.over, cls: 'fill-warn' }],
    tip: `${hourLabel(h.h)} · ${h.ok + h.over} vehicles, ${h.over} over`,
  })));
}
