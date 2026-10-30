/**
 * Preferences that belong to this phone, not to VST: kept in localStorage, never sent anywhere.
 * Sharing is off until the user turns it on and accepts the privacy note (views/system.js).
 */
const KEY = 'vst.prefs';
const DEFAULTS = { sharing: false, shareSite: true, shareTime: true, setupDone: false };

let cache;
function read() {
  if (!cache) {
    try { cache = { ...DEFAULTS, ...JSON.parse(localStorage.getItem(KEY)) }; } catch { cache = { ...DEFAULTS }; }
  }
  return cache;
}

export const prefs = {
  get: (k) => read()[k],
  set(k, v) { read()[k] = v; localStorage.setItem(KEY, JSON.stringify(cache)); },
};
