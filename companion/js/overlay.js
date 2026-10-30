/**
 * SVG drawing of VST's zones over a 1280x720 frame, shared by the Live view (read-only) and the
 * Setup editor (with drag handles).
 *
 * Config geometry follows VST.cfg: the analysis box is absolute; speed lines and obstruction are
 * x offsets from AnalysisBoxLeft; lane ("hubcap") lines are y offsets from AnalysisBoxTop.
 * Colours match the desktop tracker: R2L lane orange, L2R lane purple, speed lines white.
 *
 * `k` is frame units per CSS pixel, so strokes, labels and handles keep a constant on-screen size
 * whatever the phone width.
 */
import { esc } from './ui.js';

export const W = 1280, H = 720;
export const LANE = { R2L: '#ff9f1c', L2R: '#c084fc' };

/** Absolute frame coordinates of everything drawable, derived from config. */
export function geometry(c) {
  const x = c.AnalysisBoxLeft, y = c.AnalysisBoxTop, w = c.AnalysisBoxWidth, h = c.AnalysisBoxHeight;
  const [o0, o1] = c.obstruction || [0, 0];
  return {
    box: { x, y, w, h },
    sl: x + c.speedLineLeft, sr: x + c.speedLineRight,
    r2l: y + c.R2LStreetY, l2r: y + c.L2RStreetY,
    obs: o1 > o0 ? [x + o0, x + o1] : null,
  };
}

/** Inverse of geometry() for one dragged handle: returns the config keys it changes. */
export function applyHandle(c, handle, fx, fy) {
  const g = geometry(c);
  const clamp = (v, lo, hi) => Math.round(Math.max(lo, Math.min(hi, v)));
  const right = g.box.x + g.box.w, bottom = g.box.y + g.box.h;
  switch (handle) {
    case 'tl': {
      const nx = clamp(fx, 0, right - 200), ny = clamp(fy, 0, bottom - 60);
      // Keep the lines where they are on screen: their offsets are relative to the box corner.
      const dx = nx - g.box.x, dy = ny - g.box.y;
      return {
        AnalysisBoxLeft: nx, AnalysisBoxTop: ny, AnalysisBoxWidth: right - nx, AnalysisBoxHeight: bottom - ny,
        speedLineLeft: Math.max(0, c.speedLineLeft - dx), speedLineRight: Math.max(1, c.speedLineRight - dx),
        R2LStreetY: clamp(c.R2LStreetY - dy, 0, bottom - ny), L2RStreetY: clamp(c.L2RStreetY - dy, 0, bottom - ny),
        obstruction: g.obs ? [Math.max(0, c.obstruction[0] - dx), Math.max(0, c.obstruction[1] - dx)] : c.obstruction,
      };
    }
    case 'br': {
      const w = clamp(fx, g.box.x + 200, W - 1) - g.box.x, h = clamp(fy, g.box.y + 60, H - 1) - g.box.y;
      return {
        AnalysisBoxWidth: w, AnalysisBoxHeight: h,
        speedLineRight: Math.min(c.speedLineRight, w), speedLineLeft: Math.min(c.speedLineLeft, w - 20),
        R2LStreetY: Math.min(c.R2LStreetY, h), L2RStreetY: Math.min(c.L2RStreetY, h),
      };
    }
    case 'sl': return { speedLineLeft: clamp(fx - g.box.x, 0, c.speedLineRight - 40) };
    case 'sr': return { speedLineRight: clamp(fx - g.box.x, c.speedLineLeft + 40, g.box.w) };
    case 'r2l': return { R2LStreetY: clamp(fy - g.box.y, 0, g.box.h) };
    case 'l2r': return { L2RStreetY: clamp(fy - g.box.y, 0, g.box.h) };
    case 'o0': return { obstruction: [clamp(fx - g.box.x, 0, c.obstruction[1] - 10), c.obstruction[1]] };
    case 'o1': return { obstruction: [c.obstruction[0], clamp(fx - g.box.x, c.obstruction[0] + 10, g.box.w)] };
    default: return {};
  }
}

/** Chevron arrow path pointing along `dir`, centred at (cx, cy). */
function chevrons(cx, cy, dir, k) {
  const s = dir === 'L2R' ? 1 : -1, a = 9 * k, gap = 11 * k;
  return [-1, 0, 1].map((i) => {
    const x = cx + i * gap * s;
    return `M${x - a * s} ${cy - a}L${x} ${cy}L${x - a * s} ${cy + a}`;
  }).join('');
}

/** SVG text with a dark halo so it reads over any video. */
function textAt(k, tx, ty, text, fill = '#fff', anchor = 'start') {
  return `<text x="${tx}" y="${ty}" fill="${fill}" font-size="${11 * k}" font-weight="700" text-anchor="${anchor}" paint-order="stroke" stroke="rgba(0,0,0,.65)" stroke-width="${3 * k}" style="font-family:var(--font);letter-spacing:.04em">${esc(text)}</text>`;
}

/** Tracked vehicle boxes, white until a speed is known, then coloured against the limit. */
export function tracksSvg(tracks, { k = 2, speedLimit = 25, egregious = 40 } = {}) {
  return tracks.map((t) => {
    const col = t.speed ? (t.speed >= egregious ? '#fb7185' : t.speed > speedLimit ? '#fbbf24' : '#34d399') : '#e2e8f0';
    return `<rect x="${t.x}" y="${t.y}" width="${t.w}" height="${t.h}" rx="${4 * k}" fill="none" stroke="${col}" stroke-width="${2.4 * k}"/>`
      + (t.speed ? textAt(k, t.x + t.w / 2, t.y - 8 * k, `${t.speed} MPH`, col, 'middle') : '');
  }).join('');
}

/**
 * Inner SVG markup for the zones. Options:
 *   editable - adds drag handles (data-h attributes) for Setup;
 *   tracks   - live tracked boxes [{x,y,w,h,dir,speed}], coloured by speed against the limit.
 */
export function zonesSvg(c, { k = 2, editable = false, tracks = [], speedLimit = 25, egregious = 40 } = {}) {
  const g = geometry(c);
  const { x, y, w, h } = g.box;
  const sw = 1.6 * k;
  const label = (...a) => textAt(k, ...a);
  let s = `<defs><pattern id="hatch" width="${10 * k}" height="${10 * k}" patternTransform="rotate(45)" patternUnits="userSpaceOnUse"><rect width="${10 * k}" height="${10 * k}" fill="rgba(0,0,0,.25)"/><line x1="0" y1="0" x2="0" y2="${10 * k}" stroke="rgba(255,255,255,.45)" stroke-width="${2 * k}"/></pattern></defs>`;

  // Dim what VST ignores, then outline the analysis box.
  s += `<path d="M0 0H${W}V${H}H0Z M${x} ${y}V${y + h}H${x + w}V${y}Z" fill="rgba(5,8,12,.55)" fill-rule="evenodd"/>`;
  s += `<rect x="${x}" y="${y}" width="${w}" height="${h}" fill="none" stroke="rgba(255,255,255,.85)" stroke-width="${sw}" stroke-dasharray="${6 * k} ${4 * k}"/>`;
  s += label(x + 6 * k, y - 7 * k, 'ANALYSIS BOX');

  if (g.obs) {
    s += `<rect x="${g.obs[0]}" y="${y}" width="${g.obs[1] - g.obs[0]}" height="${h}" fill="url(#hatch)"/>`;
    s += label((g.obs[0] + g.obs[1]) / 2, y + 16 * k, 'OBSTRUCTION', '#fff', 'middle');
  }

  // Speed zone: shaded between the white lines, legal direction chevrons on top.
  s += `<rect x="${g.sl}" y="${y}" width="${g.sr - g.sl}" height="${h}" fill="rgba(94,234,212,.10)"/>`;
  for (const lx of [g.sl, g.sr]) s += `<line x1="${lx}" y1="${y}" x2="${lx}" y2="${y + h}" stroke="#fff" stroke-width="${2.2 * k}"/>`;
  s += label((g.sl + g.sr) / 2, y + h + 16 * k, 'SPEED ZONE', '#fff', 'middle');
  const legal = c.legalDirection;
  const mid = (g.sl + g.sr) / 2;
  if (legal === 'both') {
    s += `<path d="${chevrons(mid - 40 * k, y - 12 * k, 'R2L', k)}${chevrons(mid + 40 * k, y - 12 * k, 'L2R', k)}" fill="none" stroke="#5eead4" stroke-width="${2.4 * k}" stroke-linecap="round" stroke-linejoin="round"/>`;
  } else {
    s += `<path d="${chevrons(mid, y - 12 * k, legal, k)}" fill="none" stroke="#5eead4" stroke-width="${2.6 * k}" stroke-linecap="round" stroke-linejoin="round"/>`;
  }

  // Lane ("hubcap") lines.
  for (const dir of ['R2L', 'L2R']) {
    const ly = dir === 'R2L' ? g.r2l : g.l2r;
    s += `<line x1="${x}" y1="${ly}" x2="${x + w}" y2="${ly}" stroke="${LANE[dir]}" stroke-width="${sw}" stroke-dasharray="${2 * k} ${3 * k}"/>`;
    // Labels go on opposite sides of the two lines, which usually sit close together.
    if (editable) s += label(x + 6 * k, dir === 'R2L' ? ly - 6 * k : ly + 15 * k, dir === 'R2L' ? '← WHEELS' : 'WHEELS →', LANE[dir]);
  }

  s += tracksSvg(tracks, { k, speedLimit, egregious });

  if (editable) {
    const r = 9 * k, hit = 24 * k;
    const handle = (id, hx, hy, col = '#fff', shape = 'circle') => `<g class="handle" data-h="${id}">
      <circle cx="${hx}" cy="${hy}" r="${hit}" fill="transparent"/>
      ${shape === 'circle'
        ? `<circle cx="${hx}" cy="${hy}" r="${r}" fill="${col}" stroke="#0b0d10" stroke-width="${2 * k}"/>`
        : `<rect x="${hx - r * 0.7}" y="${hy - r * 1.3}" width="${r * 1.4}" height="${r * 2.6}" rx="${r * 0.6}" fill="${col}" stroke="#0b0d10" stroke-width="${2 * k}"/>`}</g>`;
    s += handle('tl', x, y);
    s += handle('br', x + w, y + h);
    s += handle('sl', g.sl, y + h / 2, '#fff', 'pill');
    s += handle('sr', g.sr, y + h / 2, '#fff', 'pill');
    // Lane handles sit at the right end, away from the box corner and the labels on the left.
    s += handle('r2l', x + w - 34 * k, g.r2l, LANE.R2L);
    s += handle('l2r', x + w - 84 * k, g.l2r, LANE.L2R);
    if (g.obs) { s += handle('o0', g.obs[0], y + h - 16 * k, '#cbd5e1', 'pill'); s += handle('o1', g.obs[1], y + h - 16 * k, '#cbd5e1', 'pill'); }
  }
  return s;
}
