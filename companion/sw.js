/**
 * Service worker: keeps the app shell available offline so the companion opens instantly and can
 * show "can't reach VST" instead of a browser error. API traffic (/api/*) is never cached: live data
 * must be live. Shell files use stale-while-revalidate, so an updated VST serves the new app on the
 * next launch. Browsers only run this on https or localhost; on a plain-http LAN the app works
 * without it.
 */
const CACHE = 'vst-companion-v1';
const SHELL = [
  './', 'index.html', 'manifest.webmanifest', 'css/app.css',
  'js/app.js', 'js/api.js', 'js/demo.js', 'js/ui.js', 'js/prefs.js', 'js/overlay.js', 'js/share.js',
  'js/views/live.js', 'js/views/passes.js', 'js/views/stats.js', 'js/views/setup.js', 'js/views/system.js', 'js/views/connect.js',
  'icons/icon.svg', 'icons/icon-192.png',
];

self.addEventListener('install', (e) => {
  e.waitUntil(caches.open(CACHE).then((c) => c.addAll(SHELL)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
  e.waitUntil(caches.keys()
    .then((keys) => Promise.all(keys.filter((k) => k !== CACHE).map((k) => caches.delete(k))))
    .then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
  const url = new URL(e.request.url);
  if (e.request.method !== 'GET' || url.origin !== location.origin || url.pathname.includes('/api/')) return;
  e.respondWith(caches.open(CACHE).then(async (cache) => {
    const hit = await cache.match(e.request, { ignoreSearch: true });
    const fresh = fetch(e.request).then((res) => {
      if (res.ok && res.type === 'basic') cache.put(e.request, res.clone());
      return res;
    }).catch(() => hit);
    return hit || fresh;
  }));
});
