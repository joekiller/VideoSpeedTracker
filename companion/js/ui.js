/**
 * Small view helpers shared by every screen: escaped HTML templating, icons, the speed sign,
 * time formatting, toasts and bottom sheets. No framework; views render strings into `main`
 * and wire events afterwards.
 */

/** Escapes a value for HTML text or attribute context. */
export const esc = (v) => String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);

/** Marks a string as already-safe HTML so `html` doesn't escape it again. */
export const raw = (s) => ({ __html: String(s) });

/**
 * Tagged template that escapes interpolations unless they are `raw()`; arrays are joined.
 * Returns a `raw` value so templates nest without double escaping.
 */
export function html(strings, ...vals) {
  const out = strings.reduce((acc, s, i) => {
    const v = vals[i - 1];
    const part = Array.isArray(v) ? v.map((x) => x?.__html ?? esc(x)).join('') : v?.__html ?? (v === false || v == null ? '' : esc(v));
    return acc + part + s;
  });
  return raw(out);
}

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => [...root.querySelectorAll(sel)];

const ICONS = {
  share: '<path d="M12 3v12M7 8l5-5 5 5"/><path d="M5 13v6a2 2 0 0 0 2 2h10a2 2 0 0 0 2-2v-6"/>',
  play: '<path d="M7 4v16l13-8z"/>',
  pause: '<path d="M8 4v16M16 4v16"/>',
  stop: '<rect x="5" y="5" width="14" height="14" rx="2"/>',
  back: '<path d="m15 5-7 7 7 7"/>',
  next: '<path d="m9 5 7 7-7 7"/>',
  up: '<path d="m5 15 7-7 7 7"/>',
  down: '<path d="m5 9 7 7 7-7"/>',
  fit: '<path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/>',
  check: '<path d="m5 12 5 5 9-10"/>',
  info: '<circle cx="12" cy="12" r="9"/><path d="M12 11v6M12 7.5v.5"/>',
  warn: '<path d="M12 3 2 20h20z"/><path d="M12 10v4M12 17v.5"/>',
  camera: '<rect x="3" y="6" width="13" height="12" rx="3"/><path d="m16 10 5-3v10l-5-3"/>',
  download: '<path d="M12 3v12M7 10l5 5 5-5"/><path d="M5 21h14"/>',
  wifi: '<path d="M2 9a15 15 0 0 1 20 0M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19.5" r="1"/>',
  lock: '<rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>',
  qr: '<rect x="3" y="3" width="7" height="7" rx="1"/><rect x="14" y="3" width="7" height="7" rx="1"/><rect x="3" y="14" width="7" height="7" rx="1"/><path d="M14 14h3v3M21 14v7h-4M14 21v-3"/>',
  refresh: '<path d="M20 11a8 8 0 1 0-2.3 5.7"/><path d="M20 4v7h-7"/>',
  car: '<path d="M5 17h14M3 13l2-6h14l2 6v4H3z"/><circle cx="7.5" cy="17" r="1.5"/><circle cx="16.5" cy="17" r="1.5"/>',
  sparkle: '<path d="M12 3v4M12 17v4M3 12h4M17 12h4M6 6l2.5 2.5M15.5 15.5 18 18M6 18l2.5-2.5M15.5 8.5 18 6"/>',
  ruler: '<path d="m3 17 14-14 4 4L7 21z"/><path d="m7 13 2 2M10 10l2 2M13 7l2 2"/>',
  compass: '<circle cx="12" cy="12" r="9"/><path d="m15.5 8.5-2 5-5 2 2-5z"/>',
  gauge: '<path d="M4 18a8 8 0 1 1 16 0"/><path d="m12 18 4.5-6"/>',
  report: '<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4M9 12h6M9 16h6"/>',
};
export const icon = (name) => raw(`<svg viewBox="0 0 24 24" aria-hidden="true">${ICONS[name] || ''}</svg>`);

/** Screen-direction arrow; R2L points left. */
export const arrow = (dir) => raw(`<span class="arrow" title="${dir === 'L2R' ? 'Left to right' : 'Right to left'}"><svg viewBox="0 0 24 24" aria-hidden="true">${dir === 'L2R' ? '<path d="M4 12h16M14 6l6 6-6 6"/>' : '<path d="M20 12H4M10 6l-6 6 6 6"/>'}</svg></span>`);

/** Status band for a pass, ordered by severity: wrong-way beats way-too-fast beats speeding. */
export function band(v) {
  if (v.wrongWay) return 'wrong';
  if (v.egregious) return 'bad';
  if (v.speeding) return 'warn';
  return 'ok';
}
export const BAND_LABEL = { ok: 'Under limit', warn: 'Speeding', bad: 'Way too fast', wrong: 'Wrong way' };

/** The speed sign tile. */
export const sign = (speed, cls = 'ok', label = 'MPH', size = '') =>
  html`<div class="sign ${cls} ${size}" role="img" aria-label="${speed} miles per hour"><small>${label === 'MPH' ? 'SPEED' : label}</small><b>${speed}</b><small>MPH</small></div>`;

/** Pills that explain a pass in words, so status is never colour alone. */
export function passPills(v, limit) {
  const p = [];
  if (v.wrongWay) p.push(html`<span class="wrongway">WRONG WAY</span>`);
  if (v.egregious) p.push(html`<span class="pill bad">Way too fast</span>`);
  else if (v.speeding) p.push(html`<span class="pill warn">+${v.speed - limit} over</span>`);
  if (v.large) p.push(html`<span class="pill">Large</span>`);
  return p;
}

// ---- time -------------------------------------------------------------------------------------

const rtf = new Intl.RelativeTimeFormat(undefined, { numeric: 'auto' });
export function ago(iso) {
  const s = (Date.parse(iso) - Date.now()) / 1000;
  const a = Math.abs(s);
  if (a < 45) return 'just now';
  if (a < 3600) return rtf.format(Math.round(s / 60), 'minute');
  if (a < 86400) return rtf.format(Math.round(s / 3600), 'hour');
  if (a < 30 * 86400) return rtf.format(Math.round(s / 86400), 'day');
  // Older than a month (a recorded video from years back): the date reads better than "3,928 days ago".
  return new Date(iso).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
}
export const clock = (iso) => new Date(iso).toLocaleTimeString(undefined, { hour: 'numeric', minute: '2-digit' });
export const dateLong = (iso) => new Date(iso).toLocaleString(undefined, { weekday: 'short', month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit' });
export function dayLabel(d) {
  const today = new Date(); today.setHours(0, 0, 0, 0);
  const that = new Date(d); that.setHours(0, 0, 0, 0);
  const diff = Math.round((today - that) / 86400e3);
  if (diff === 0) return 'Today';
  if (diff === 1) return 'Yesterday';
  return that.toLocaleDateString(undefined, { weekday: 'long', month: 'short', day: 'numeric' });
}

// ---- toasts & sheets --------------------------------------------------------------------------

/** Shows a transient toast; `content` is html. Optional href makes it a link. */
export function toast(content, { href, ms = 4500 } = {}) {
  const host = $('#toasts');
  const el = document.createElement(href ? 'a' : 'div');
  el.className = 'toast';
  if (href) el.href = href;
  el.innerHTML = content.__html ?? esc(content);
  host.append(el);
  setTimeout(() => { el.classList.add('out'); setTimeout(() => el.remove(), 300); }, ms);
}

/**
 * Opens a bottom sheet with `content` html. Resolves with whatever `close(value)` is called with
 * (from a button wired by `onOpen`), or undefined when dismissed by the backdrop or Escape.
 */
export function sheet(content, onOpen) {
  return new Promise((resolve) => {
    const back = document.createElement('div');
    back.className = 'sheet-backdrop';
    back.innerHTML = `<div class="sheet" role="dialog" aria-modal="true">${content.__html}</div>`;
    // Keyboard and screen-reader users land in the sheet, named by its heading, and go back where they were.
    const opener = document.activeElement;
    const dlg = back.firstElementChild, title = dlg.querySelector('h2');
    if (title) { title.id ||= `sheet-title-${Date.now()}`; dlg.setAttribute('aria-labelledby', title.id); }
    const close = (val) => { back.remove(); document.removeEventListener('keydown', onKey); opener?.focus?.(); resolve(val); };
    const onKey = (e) => e.key === 'Escape' && close();
    back.addEventListener('click', (e) => e.target === back && close());
    document.addEventListener('keydown', onKey);
    document.body.append(back);
    onOpen?.(dlg, close);
    dlg.querySelector('.btn.primary, button')?.focus({ preventScroll: true });
  });
}

/** Simple confirm sheet. */
export const confirmSheet = (title, body, okLabel = 'OK', danger = false) => sheet(html`
  <h2>${title}</h2><p>${body}</p>
  <div class="btn-row"><button class="btn" data-v="0">Cancel</button><button class="btn ${danger ? 'danger' : 'primary'}" data-v="1">${okLabel}</button></div>`,
(el, close) => $$('[data-v]', el).forEach((b) => b.onclick = () => close(b.dataset.v === '1')));
