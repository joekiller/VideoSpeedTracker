/**
 * System: run controls, what VST is doing, this phone's connection, and the sharing opt-in.
 *
 * Sharing is a per-phone choice stored locally (prefs.js). Turning it on shows what sharing means
 * (clips can show plates and faces; nothing is uploaded unless you pick an app to share to).
 */
import { html, $, icon, sheet, $$, confirmSheet, toast } from '../ui.js';

export async function render({ vst, view, prefs, start }) {
  const paint = () => {
    const s = vst.lastStatus;
    const c = s.counts || {};
    const src = s.source || {};
    const live = vst.mode !== 'demo';
    const where = live ? (vst.base || location.origin) : 'Built-in demo';
    // A repaint replaces every control; put keyboard focus back on the one that had it.
    const had = view.contains(document.activeElement) ? document.activeElement : null;
    const refocus = had && (had.id ? `#${had.id}` : had.dataset.a ? `[data-a="${had.dataset.a}"]` : had.dataset.pref ? `[data-pref="${had.dataset.pref}"]` : null);
    view.innerHTML = html`
      <h1>System</h1>
      <p class="lede">${s.running ? (s.paused ? 'Tracking is paused.' : 'VST is watching the street.') : 'Tracking is stopped.'}</p>
      <div class="btn-row" id="run">
        ${s.running
          ? html`<button class="btn" data-a="${s.paused ? 'resume' : 'pause'}">${icon(s.paused ? 'play' : 'pause')}${s.paused ? 'Resume' : 'Pause'}</button>
                 <button class="btn danger" data-a="stop">${icon('stop')}Stop</button>`
          : html`<button class="btn primary" data-a="start">${icon('play')}Start tracking</button>`}
      </div>

      <h3>Camera</h3>
      <div class="list">
        ${s.source
          ? html`<div class="item"><div class="label"><b>${src.name || 'Source'}</b><small>${{ file: 'Video file', camera: 'Camera', stream: 'Network stream', image: 'Photo' }[src.kind] || ''}</small></div><div class="value">${src.width ? `${src.width}×${src.height} · ${Math.round(src.fps)} fps` : 'Connecting…'}</div></div>`
          : html`<div class="item"><div class="label"><b>No camera connected</b><small>Pick one in VST Studio, step 3: Connect the camera</small></div><div class="value">—</div></div>`}
        <div class="item"><div class="label"><b>Processing</b><small>Frames analysed per second</small></div><div class="value">${s.running && !s.paused ? `${(s.procFps || 0).toFixed(1)} fps` : '—'}</div></div>
        <div class="item"><div class="label"><b>Frame</b></div><div class="value">${(s.frame || 0).toLocaleString()}${s.totalFrames ? ` of ${s.totalFrames.toLocaleString()}` : ''}</div></div>
        <div class="item"><div class="label"><b>This session</b><small>Since VST started</small></div><div class="value">${c.vehicles ?? 0} vehicles · top ${c.topSpeed ?? 0} mph</div></div>
        <div class="item"><div class="label"><b>VST version</b></div><div class="value">${s.version}</div></div>
      </div>

      <h3>Sharing</h3>
      <div class="list">
        <div class="item"><div class="label"><b>Allow sharing from this phone</b><small>Off by default. Nothing is uploaded unless you share it.</small></div>
          <label class="switch"><input type="checkbox" id="sharing" aria-label="Allow sharing from this phone" ${prefs.get('sharing') ? 'checked' : ''}><i></i></label></div>
        ${prefs.get('sharing') ? html`
          <div class="item"><div class="label"><b>Include the spot's name</b><small>${s.siteName || 'Not set'}</small></div>
            <label class="switch"><input type="checkbox" data-pref="shareSite" aria-label="Include the spot's name" ${prefs.get('shareSite') ? 'checked' : ''}><i></i></label></div>
          <div class="item"><div class="label"><b>Include date and time</b></div>
            <label class="switch"><input type="checkbox" data-pref="shareTime" aria-label="Include date and time" ${prefs.get('shareTime') ? 'checked' : ''}><i></i></label></div>` : ''}
      </div>

      <h3>Connection</h3>
      <div class="list">
        <div class="item"><div class="label"><b>${live ? 'Connected to' : 'Mode'}</b><small>${live ? 'Local network only' : 'Simulated traffic, nothing real'}</small></div><div class="value">${where}</div></div>
        <div class="item"><div class="label"><b>Install on your home screen</b><small>${installHint()}</small></div></div>
      </div>
      <div class="btn-row" style="margin-top:12px">
        ${live ? html`<button class="btn" id="demo">${icon('sparkle')}Try the demo</button>` : html`<button class="btn primary" id="real">${icon('wifi')}Connect to my VST</button>`}
        <button class="btn danger" id="forget">${live ? 'Unpair this phone' : 'Leave demo'}</button>
      </div>
      <p class="small muted" style="margin-top:24px">VST Companion is part of <a href="https://github.com/joekiller/VideoSpeedTracker">VideoSpeedTracker</a>, open source under the BSD license. Data stays on your network.</p>`.__html;

    if (refocus) view.querySelector(refocus)?.focus();
    $('#run').onclick = async (e) => {
      const a = e.target.closest('[data-a]')?.dataset.a;
      if (!a) return;
      if (a === 'stop' && !(await confirmSheet('Stop tracking?', 'VST stops measuring until someone starts it again, from here or on the computer.', 'Stop', true))) return;
      await vst.run(a); await vst.status(); paint();
    };
    $('#sharing').onchange = async (e) => {
      if (!e.target.checked) { prefs.set('sharing', false); paint(); return; }
      e.target.checked = false;
      const ok = await sheet(html`
        <h2>Before you share</h2>
        <div class="check">${icon('camera')}<p><b>Clips and snapshots can show plates, faces and homes.</b> Look before you post, and think about who is in the picture.</p></div>
        <div class="check">${icon('lock')}<p><b>Nothing is uploaded by VST.</b> Share opens your phone's share sheet; it goes only where you send it.</p></div>
        <div class="check">${icon('gauge')}<p><b>Speeds are estimates.</b> Cards say "measured with VideoSpeedTracker", not that anyone broke the law.</p></div>
        <div class="btn-row" style="margin-top:16px"><button class="btn" data-v="0">Not now</button><button class="btn primary" data-v="1">Turn on sharing</button></div>`,
      (el, close) => $$('[data-v]', el).forEach((b) => b.onclick = () => close(b.dataset.v === '1')));
      if (ok) { prefs.set('sharing', true); toast('Sharing is on for this phone'); }
      paint();
    };
    $$('[data-pref]').forEach((i) => i.onchange = () => prefs.set(i.dataset.pref, i.checked));
    $('#demo')?.addEventListener('click', () => { vst.useDemo(); start(); });
    $('#real')?.addEventListener('click', () => { vst.mode = 'live'; vst.save(); location.hash = '#/connect'; start(); });
    $('#forget').onclick = async () => {
      if (live && !(await confirmSheet('Unpair this phone?', 'You will need to scan the QR code in VST Studio again to reconnect.', 'Unpair', true))) return;
      vst.forget(); start();
    };
  };

  await vst.status();
  paint();
  const onStatus = () => { if (!document.querySelector('.sheet')) paintCounts(); };
  // Repaint only the live numbers each second, so open switches and focus aren't disturbed.
  const paintCounts = () => {
    const items = view.querySelectorAll('.list')[0]?.querySelectorAll('.item .value');
    const s = vst.lastStatus;
    if (!items) return;
    items[1].textContent = s.running && !s.paused ? `${(s.procFps || 0).toFixed(1)} fps` : '—';
    items[2].textContent = `${(s.frame || 0).toLocaleString()}${s.totalFrames ? ` of ${s.totalFrames.toLocaleString()}` : ''}`;
  };
  vst.addEventListener('status', onStatus);
  return () => vst.removeEventListener('status', onStatus);
}

function installHint() {
  if (matchMedia('(display-mode: standalone)').matches) return 'Installed. Open it from your home screen.';
  if (/iphone|ipad/i.test(navigator.userAgent)) return 'In Safari, tap Share, then "Add to Home Screen".';
  return 'In your browser menu, choose "Install app" or "Add to Home screen".';
}
