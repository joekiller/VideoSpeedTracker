/**
 * Connect: shown when the phone isn't paired with a reachable VST. Explains the three-step pairing
 * (turn on the companion in VST Studio, scan the QR code), accepts an address and code by hand, and offers the
 * demo so anyone can see what the companion does before setting anything up.
 */
import { html, $, icon } from '../ui.js';

const WHY = {
  unreachable: 'No VST answered. Check that the computer is on, VST Studio is open with the companion turned on, and this phone is on the same Wi-Fi.',
  'needs-token': 'Found VST, but this phone isn\'t paired yet (or was unpaired). Scan the QR code again, or type the pairing code.',
};

export function render({ vst, view, start, probeResult }) {
  // Warn only after a real attempt: an address or code was entered or came from a QR link. Opening the page
  // VST serves (port 8787) with no code yet is the normal first visit, not a failure.
  const tried = probeResult && vst.mode === 'live' && (vst.base || vst.token);
  view.innerHTML = html`
    <div class="connect-hero">
      <svg class="big" viewBox="0 0 96 96" aria-hidden="true">
        <defs><linearGradient id="cg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#5eead4"/><stop offset="1" stop-color="#0d9488"/></linearGradient></defs>
        <rect x="6" y="6" width="84" height="84" rx="24" fill="url(#cg)"/>
        <path d="M24 64a24 24 0 1 1 48 0" fill="none" stroke="#062c27" stroke-width="7" stroke-linecap="round"/>
        <path d="m48 64 14-17" stroke="#062c27" stroke-width="7" stroke-linecap="round"/><circle cx="48" cy="64" r="5" fill="#062c27"/>
      </svg>
      <h1>Your street, in your pocket</h1>
      <p class="lede">Watch the camera, see who is speeding or going the wrong way, and calibrate VST from your phone.</p>
    </div>
    ${tried ? html`<div class="callout warn" style="margin-bottom:16px">${icon('warn')}<p>${WHY[probeResult] || WHY.unreachable}</p></div>` : ''}
    <h3>Pair with VST</h3>
    <div class="card">
      <div class="check"><b class="num" style="color:var(--accent)">1</b><p>On the computer running VST Studio, open <b>Companion app</b> (left sidebar, under Share) and turn it on.</p></div>
      <div class="check"><b class="num" style="color:var(--accent)">2</b><p><b>Scan the QR code</b> it shows with this phone's camera. That opens the companion already paired.</p></div>
      <div class="check"><b class="num" style="color:var(--accent)">3</b><p><b>Add it to your home screen</b> so it opens like an app next time.</p></div>
    </div>
    <p class="small muted" style="margin:4px 0 8px">Is this phone the camera? Pair a second phone or a tablet instead, or skip the companion: every step works in VST Studio on the computer.</p>
    <details style="margin-top:4px" ${tried ? 'open' : ''}><summary class="small muted">Can't scan? Enter it by hand</summary>
      <form class="card stack" id="manual" style="margin-top:4px">
        <label class="field"><span>Address shown under the QR code</span><input class="input" id="addr" inputmode="url" autocomplete="off" placeholder="192.168.1.20:8787" value="${vst.base.replace(/^https?:\/\//, '')}"></label>
        <label class="field"><span>Pairing code (under the address in VST Studio)</span><input class="input" id="tok" autocomplete="off" autocapitalize="off" spellcheck="false" value="${vst.token}"></label>
        <button class="btn primary block" type="submit">${icon('wifi')}Connect</button>
        <p class="small" id="msg" role="status" style="margin:0"></p>
      </form>
    </details>
    <h3>Just looking?</h3>
    <button class="btn block" id="demo">${icon('sparkle')}Explore the demo</button>
    <p class="small muted" style="margin-top:8px;text-align:center">Simulated traffic on a one-way street, with video from the original VST project.</p>`.__html;

  $('#demo').onclick = () => { vst.useDemo(); start(); };
  $('#manual').onsubmit = async (e) => {
    e.preventDefault();
    $('#msg').textContent = 'Connecting…';
    try { vst.useLive($('#addr').value, $('#tok').value.replace(/\s+/g, '')); } catch { $('#msg').textContent = 'That address doesn\'t look right.'; return; }
    if (location.protocol === 'https:' && vst.base.startsWith('http:')) {
      $('#msg').textContent = 'This page is on https, which can\'t reach VST over plain http. Open the address above directly instead.';
      return;
    }
    const r = await vst.probe();
    if (r === 'ok') start();
    else $('#msg').textContent = WHY[r];
  };
}
