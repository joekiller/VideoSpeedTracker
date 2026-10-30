/**
 * Opt-in sharing. Nothing leaves the phone unless the user has turned sharing on (System screen) and
 * then taps Share; even then it goes through the phone's own share sheet (Web Share API) to whatever
 * app they pick, or downloads as a file where sharing files isn't supported. There is no server.
 *
 * Cards are drawn on a canvas at 1080x1350 (the portrait size most social apps show uncropped):
 *   passCard   - one pass: its snapshot, the speed sign against the limit, wrong-way banner;
 *   reportCard - the neighbourhood summary from the Stats screen.
 */
import { prefs } from './prefs.js';

const CW = 1080, CH = 1350;
const C = { bg: '#0b0d10', panel: '#13161b', text: '#eef1f5', muted: '#8a94a3', ok: '#34d399', warn: '#fbbf24', bad: '#fb7185', wrong: '#e879f9', accent: '#5eead4' };
const FONT = 'system-ui, -apple-system, "Segoe UI", Roboto, sans-serif';

export const sharingOn = () => !!prefs.get('sharing');

function canvas() {
  const c = document.createElement('canvas');
  c.width = CW; c.height = CH;
  const g = c.getContext('2d');
  g.fillStyle = C.bg; g.fillRect(0, 0, CW, CH);
  return [c, g];
}

const loadImg = (src) => new Promise((res) => {
  if (!src) return res(null);
  const i = new Image();
  i.crossOrigin = 'anonymous';
  i.onload = () => res(i);
  i.onerror = () => res(null);
  i.src = src;
});

function roundRect(g, x, y, w, h, r) { g.beginPath(); g.roundRect(x, y, w, h, r); }

/** Regulatory-style speed sign, `label` on top (e.g. SPEED LIMIT / CLOCKED), rimmed in `rim`. */
function drawSign(g, x, y, w, label, value, rim) {
  const h = w * 1.22;
  if (rim) { g.fillStyle = rim; roundRect(g, x - 10, y - 10, w + 20, h + 20, 30); g.fill(); }
  g.fillStyle = '#f8fafc'; roundRect(g, x, y, w, h, 22); g.fill();
  g.strokeStyle = '#0b0d10'; g.lineWidth = 8; roundRect(g, x + 12, y + 12, w - 24, h - 24, 14); g.stroke();
  g.fillStyle = '#0b0d10'; g.textAlign = 'center';
  label.split('\n').forEach((line, i) => { g.font = `800 ${w * 0.12}px ${FONT}`; g.fillText(line, x + w / 2, y + w * 0.24 + i * w * 0.14); });
  g.font = `850 ${w * 0.48}px ${FONT}`;
  g.fillText(String(value), x + w / 2, y + h * 0.8);
  g.textAlign = 'left';
}

function footer(g, site, when) {
  g.textAlign = 'left';
  g.fillStyle = C.muted; g.font = `600 30px ${FONT}`;
  const parts = [prefs.get('shareSite') && site, prefs.get('shareTime') && when].filter(Boolean);
  if (parts.length) g.fillText(parts.join('  ·  '), 72, CH - 120);
  g.fillStyle = C.accent; g.font = `700 30px ${FONT}`;
  g.fillText('Measured with VideoSpeedTracker', 72, CH - 72);
}

/** Draws `img` to fill (x,y,w,h), cropping the excess like CSS object-fit: cover. */
function cover(g, img, x, y, w, h, r = 28) {
  g.save(); roundRect(g, x, y, w, h, r); g.clip();
  const s = Math.max(w / img.width, h / img.height);
  g.drawImage(img, x + (w - img.width * s) / 2, y + (h - img.height * s) / 2, img.width * s, img.height * s);
  g.restore();
}

/** Card for one pass. `snapshotUrl` must be same-origin (it is: VST serves both). */
export async function passCard(v, { speedLimit, siteName }, snapshotUrl) {
  const [c, g] = canvas();
  const img = await loadImg(snapshotUrl);
  if (img) cover(g, img, 48, 48, CW - 96, 520);
  const status = v.wrongWay ? C.wrong : v.egregious ? C.bad : v.speeding ? C.warn : C.ok;

  // Two signs side by side: the posted limit and what this vehicle did.
  drawSign(g, 120, 640, 300, 'SPEED\nLIMIT', speedLimit, null);
  drawSign(g, CW - 420, 640, 300, 'CLOCKED\nAT', v.speed, status);

  g.textAlign = 'left';
  if (v.wrongWay) {
    g.fillStyle = '#dc2626'; roundRect(g, 72, 1050, 520, 96, 12); g.fill();
    g.strokeStyle = '#fff'; g.lineWidth = 6; roundRect(g, 82, 1060, 500, 76, 8); g.stroke();
    g.fillStyle = '#fff'; g.font = `900 56px ${FONT}`; g.textAlign = 'center';
    g.fillText('WRONG WAY', 332, 1120);
  } else {
    g.fillStyle = C.text; g.font = `800 64px ${FONT}`;
    const over = v.speed - speedLimit;
    g.fillText(over > 0 ? `${over} mph over the limit` : 'Within the limit', 72, 1120);
  }
  footer(g, siteName, new Date(v.time).toLocaleString(undefined, { dateStyle: 'medium', timeStyle: 'short' }));
  return c;
}

/** Summary card: 85th percentile, share speeding, counts, and a small speed histogram. */
export function reportCard(s, { speedLimit, siteName, period }) {
  const [c, g] = canvas();
  g.textAlign = 'left';
  g.fillStyle = C.accent; g.font = `700 34px ${FONT}`; g.fillText(`TRAFFIC REPORT · ${period.toUpperCase()}`, 72, 120);
  g.fillStyle = C.text; g.font = `800 72px ${FONT}`; g.fillText(siteName && prefs.get('shareSite') ? siteName : 'Our street', 72, 210);

  drawSign(g, 72, 290, 260, 'SPEED\nLIMIT', speedLimit, null);
  g.fillStyle = C.text; g.font = `850 200px ${FONT}`; g.fillText(String(s.p85), 400, 500);
  const w = g.measureText(String(s.p85)).width;
  g.fillStyle = C.muted; g.font = `700 48px ${FONT}`; g.fillText('mph', 410 + w, 500);
  g.font = `600 34px ${FONT}`; g.fillText('85% of drivers go this fast or slower', 400, 560);

  const tiles = [[`${s.pctSpeeding}%`, 'over the limit', C.warn], [String(s.n), 'vehicles measured', C.text],
    [String(s.egregious), `at ${s.egregiousAt}+ mph`, C.bad], [s.oneWay ? String(s.wrong) : `${s.top}`, s.oneWay ? 'wrong-way drivers' : 'top speed (mph)', s.oneWay ? C.wrong : C.text]];
  tiles.forEach(([v, l, col], i) => {
    const x = 72 + (i % 2) * 476, y = 660 + Math.floor(i / 2) * 190;
    g.fillStyle = C.panel; roundRect(g, x, y, 460, 170, 24); g.fill();
    g.fillStyle = col; g.font = `800 76px ${FONT}`; g.fillText(v, x + 32, y + 92);
    g.fillStyle = C.muted; g.font = `600 30px ${FONT}`; g.fillText(l, x + 32, y + 140);
  });

  // Mini histogram of speeds, bars coloured by band, limit marked.
  const bins = s.histogram, max = Math.max(1, ...bins.map((b) => b.n));
  const x0 = 72, y0 = 1170, bw = (CW - 144) / bins.length, hgt = 120;
  bins.forEach((b, i) => {
    const bh = (b.n / max) * hgt;
    g.fillStyle = b.speed >= s.egregiousAt ? C.bad : b.speed > speedLimit ? C.warn : '#3b4552';
    roundRect(g, x0 + i * bw + 1, y0 - bh, Math.max(1, bw - 2), bh, 3); g.fill();
  });
  footer(g, null, null);
  return c;
}

/** Shares a canvas as PNG through the share sheet, or downloads it. */
export async function shareCanvas(c, name, text) {
  const blob = await new Promise((r) => c.toBlob(r, 'image/png'));
  return shareBlob(blob, `${name}.png`, text);
}

/** Fetches a clip and shares it as a video file. */
export async function shareClip(url, name, text) {
  const blob = await fetch(url).then((r) => { if (!r.ok) throw new Error('Clip not available'); return r.blob(); });
  return shareBlob(new Blob([blob], { type: 'video/mp4' }), `${name}.mp4`, text);
}

async function shareBlob(blob, filename, text) {
  const file = new File([blob], filename, { type: blob.type });
  if (navigator.canShare?.({ files: [file] })) {
    try { await navigator.share({ files: [file], text }); return 'shared'; } catch (e) { if (e.name === 'AbortError') return 'cancelled'; }
  }
  const a = Object.assign(document.createElement('a'), { href: URL.createObjectURL(blob), download: filename });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 10e3);
  return 'downloaded';
}
