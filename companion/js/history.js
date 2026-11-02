/**
 * Where Passes and Stats get their passes from, and the calibration sets those passes belong to.
 *
 * Two scopes (docs/companion-api.md, GET /api/vehicles):
 *   - "saved": every run VST Studio has saved (Data/Stats), the same list as Studio's Results "All saved runs", so the
 *     street's history is still here after Studio restarts. Each pass carries `run`, `set`, `measured`, `adjusted`
 *     and `calibration`; only passes from this session keep an `id`, a snapshot and a clip.
 *   - "session": what VST measured since Studio last opened, with pictures (the API's original list).
 * The choice belongs to this phone (prefs) and is shared by both screens. A VST Studio too old to know scope=saved
 * has no GET /api/sets either: then only "session" is offered.
 *
 * Calibration sets (src/Gui/CalSets.h): runs measured from the same camera view share a set. An open set's speeds
 * follow the latest calibration; a locked one keeps the calibration it was measured with; a published one is frozen
 * for a report. The phone shows them the way Results does: a coloured band down each pass's left edge (colour by set
 * number, `stripe`), and a strip of set chips that filters to one set. Locking and publishing stay on the desktop.
 */
import { html, icon } from './ui.js';
import { prefs } from './prefs.js';

export const SCOPES = { saved: 'All saved runs', session: 'This session' };
/** Passes asked for in the saved scope: plenty for a phone, and the Street report covers more. */
export const SAVED_LIMIT = 20000;

let support = null;   // null until known; then { saved: bool, sets: [...] }
let pickedSet = 0;    // 0 = every set; else the set id the chips filter to (shared by Passes and Stats)

/** The scope on screen: the phone's choice, or "session" when this VST Studio has no saved scope. */
export function scope() {
  const s = prefs.get('scope') || 'saved';
  return support && !support.saved ? 'session' : s;
}
export function setScope(s) { prefs.set('scope', s); pickedSet = 0; }
export const picked = () => pickedSet;
export function pick(id) { pickedSet = pickedSet === id ? 0 : id; }

/**
 * Learns whether VST Studio serves saved runs and reads its calibration sets (re-read each time a screen opens, as
 * a lock or a new run on the desktop changes them). Resolves to the sets, [] when there are none or no support.
 */
export async function loadSets(vst) {
  try {
    const sets = await vst.sets();
    support = { saved: true, sets };
  } catch (e) {
    // 404: a VST Studio from before saved runs. Anything else (offline, busy) leaves what was known.
    if (e.status === 404) support = { saved: false, sets: [] };
    else support ??= { saved: true, sets: [] };
  }
  if (pickedSet && !support.sets.some((s) => s.id === pickedSet)) pickedSet = 0;
  return support.sets;
}
export const savedSupported = () => !support || support.saved;
export const setById = (id) => support?.sets.find((s) => s.id === id) ?? null;
/** The open set that follows today's calibration: the one a pass logged right now joins. */
export const currentSet = () => support?.sets.find((s) => s.followsCalibration) ?? null;

/** Fetches passes for the scope. `params` as for vst.vehicles (from, limit). */
export function passes(vst, params = {}) {
  if (scope() === 'saved') return vst.vehicles({ ...params, scope: 'saved', set: pickedSet || null });
  return vst.vehicles(params);
}

/**
 * A pass announced live (`vehicle` event) as a saved-scope item: it is in this session's run, which belongs to the
 * open set on today's view (Studio files it there when the run's manifest is read).
 */
export function asSaved(v) {
  return { ...v, run: null, set: currentSet()?.id ?? null, measured: v.speed, adjusted: false, calibration: null };
}

/**
 * Where a pass opens. A saved pass opens by its run and frame even when it is also in this session, so its detail shows
 * the same (re-timed) speed and marks as the list; the saved item still carries the session's picture and id. A pass
 * with no run (this session's list, or one that just arrived live) opens by id.
 */
export const passHref = (v) => (v.run ? `#/passes/saved/${encodeURIComponent(v.run)}/${v.frame}` : `#/passes/${v.id}`);

/** The CSS class for a pass's set band ('' when it has none, e.g. in the session scope). */
export function stripeClass(setId) {
  const s = setId ? setById(setId) : null;
  if (!s) return '';
  return s.stripe == null ? 'set-band set-unknown' : `set-band set-s${s.stripe}`;
}

/** A set's standing in a few words and one plain sentence, for the chips' caption and a pass's detail. */
export function setWords(s) {
  if (!s.known) return { state: 'As measured', icon: 'info', line: 'Saved before VST recorded calibrations, so these speeds are shown exactly as measured.' };
  if (s.published) return { state: 'Published', icon: 'lock', line: 'Published for a report: frozen at the calibration it was measured with, and never changed.' };
  if (s.state === 'locked') {
    const why = s.lockReason === 'view' ? 'Locked when the camera view changed' : 'Locked in VST Studio';
    const newer = s.newerCalibration ? ' A newer calibration exists; you can apply it to this set in VST Studio\'s Results.' : '';
    return { state: 'Locked', icon: 'lock', line: `${why}: its speeds stay at the calibration they were measured with.${newer}` };
  }
  if (s.followsCalibration) return { state: 'Open', icon: 'ruler', line: 'Open: its speeds follow your latest calibration, so a better calibration improves them all.' };
  return { state: 'Open', icon: 'ruler', line: 'Open, from a camera view that isn\'t today\'s.' };
}

/** The scope switch (segmented control). Hidden when only the session is available. */
export function scopeSeg() {
  if (!savedSupported()) return '';
  return html`<div class="seg scope" id="scope" role="group" aria-label="Which passes">${Object.entries(SCOPES).map(([k, l]) =>
    html`<button data-scope="${k}" aria-pressed="${scope() === k}">${l}</button>`)}</div>`;
}

/**
 * The calibration-set chips (every set with saved runs, oldest first), with the picked set's sentence below. Picking
 * one reloads with `set=` so a set older than the newest few thousand passes still shows in full. Nothing when the
 * scope is the session, or there is a single open set (nothing to tell apart).
 */
export function setsStrip() {
  if (scope() !== 'saved' || !support?.sets.length) return '';
  const here = support.sets.filter((s) => s.runs > 0);
  const plain = (s) => s.known && s.state === 'open' && !s.published;
  if (!here.length || (here.length === 1 && plain(here[0]))) return '';
  const p = pickedSet ? here.find((s) => s.id === pickedSet) : null;
  const words = p ? setWords(p) : null;
  return html`
    <div class="sets" id="sets">
      <div class="sets-head"><span class="kicker">${here.length === 1 ? 'Calibration set' : 'Calibration sets'}</span>
        <details class="sets-help"><summary>What's this?</summary>
          <p>Passes measured from the same camera view share a <b>set</b>. The coloured band down each pass shows its set.
          An <b>open</b> set's speeds follow your latest calibration. A <b>locked</b> set keeps the calibration it was
          measured with, so moving the camera never changes old numbers. Tap a set to see only its passes.</p></details></div>
      <div class="chips">${here.map((s) => {
        const w = setWords(s);
        return html`<button class="chip" data-set="${s.id}" aria-pressed="${pickedSet === s.id}" title="${s.summary}">
          <i class="${stripeClass(s.id)}"></i>Set ${s.id}${s.published || s.state === 'locked' ? html`<span class="chip-icon">${icon('lock')}</span>` : ''}<small>${w.state}</small></button>`;
      })}</div>
      ${words ? html`<p class="sets-line"><b>${p.summary}</b><br>${words.line}</p>`
        : html`<p class="sets-line muted">${here.some((s) => s.state === 'locked') ? 'Each set is shown at its own calibration.' : 'Every set follows your latest calibration.'} Tap a set to see only its passes.</p>`}
    </div>`;
}

/** Keeps the picked set's passes, or all of them. */
export const inPicked = (items) => (pickedSet && scope() === 'saved' ? items.filter((v) => v.set === pickedSet) : items);

/** "yyyy-mm-ddThh:mm:ss" local time, what VST compares `from`/`to` against (its times are local too). */
export function localStamp(ms) {
  const d = new Date(ms);
  const p = (n) => String(n).padStart(2, '0');
  return `${d.getFullYear()}-${p(d.getMonth() + 1)}-${p(d.getDate())}T${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}
