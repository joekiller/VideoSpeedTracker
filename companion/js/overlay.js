/**
 * SVG drawing of VST's zones over a 1280x720 frame, shared by the Live view (read-only) and the
 * Setup editor (with drag handles).
 *
 * Config geometry follows VST.cfg: the analysis box is absolute; speed lines and obstruction are
 * x offsets from AnalysisBoxLeft; lane ("hubcap") lines are y offsets from AnalysisBoxTop, at the box's left edge
 * (R2LStreetY / L2RStreetY) and right edge (R2LStreetYRight / L2RStreetYRight, 0 = level), so a lane can run at an
 * angle across the picture. streetEdge [left, right] ([0, 0] = off) is a line along the curb that VST Studio uses
 * to measure how the street recedes; the phone shows it but leaves its editing to Studio.
 * Colours match the desktop tracker: R2L lane orange, L2R lane purple, speed lines white.
 *
 * `k` is frame units per CSS pixel, so strokes, labels and handles keep a constant on-screen size
 * whatever the phone width.
 */
import { esc } from './ui.js';

export const W = 1280, H = 720;
export const LANE = { R2L: '#ff9f1c', L2R: '#c084fc' };
export const EDGE = '#84cc16';

/** Absolute frame coordinates of everything drawable, derived from config. */
export function geometry(c) {
  const x = c.AnalysisBoxLeft, y = c.AnalysisBoxTop, w = c.AnalysisBoxWidth, h = c.AnalysisBoxHeight;
  const [o0, o1] = c.obstruction || [0, 0];
  return {
    box: { x, y, w, h },
    sl: x + c.speedLineLeft, sr: x + c.speedLineRight,
    r2l: y + c.R2LStreetY, l2r: y + c.L2RStreetY,
    // Right ends: the same as the left for a level line.
    r2lR: y + (c.R2LStreetYRight || c.R2LStreetY), l2rR: y + (c.L2RStreetYRight || c.L2RStreetY),
    obs: o1 > o0 ? [x + o0, x + o1] : null,
    edge: c.streetEdge && c.streetEdge[0] > 0 && c.streetEdge[1] > 0 ? [y + c.streetEdge[0], y + c.streetEdge[1]] : null,
  };
}

/**
 * The speed zone's rails, the same as VST's (Globals::railZone, Globals::kMinZone): the lines sit inside the box, at
 * least MIN_ZONE px apart, so a car is seen several times between them. A line may sit right on the box's side; VST
 * times cars as they come into view there. keepLeft says which line (or box side) the user just moved; the other one
 * gives way. Returns the two line keys.
 */
export const MIN_ZONE = 200;
export function railZone(c, keepLeft) {
  const w = c.AnalysisBoxWidth, clamp = (v, lo, hi) => Math.max(lo, Math.min(hi, v));
  let sl = c.speedLineLeft, sr = c.speedLineRight;
  if (keepLeft) {
    sl = clamp(sl, 0, Math.max(w - MIN_ZONE, 0));
    sr = clamp(sr, Math.min(sl + MIN_ZONE, w), w);
  } else {
    sr = clamp(sr, Math.min(MIN_ZONE, w), w);
    sl = clamp(sl, 0, Math.max(sr - MIN_ZONE, 0));
  }
  return { speedLineLeft: Math.round(sl), speedLineRight: Math.round(sr) };
}

/** A lane line's y at frame x, between its left end yl and right end yr across the box. */
export function laneYAt(g, yl, yr, fx) {
  return yl + (yr - yl) * (fx - g.box.x) / (g.box.w || 1);
}

/** Where the phone's lane handles sit: near the right end, on the (maybe angled) line. */
export function laneHandleX(g, dir, k) {
  return g.box.x + g.box.w - (dir === 'R2L' ? 34 : 84) * k;
}

/**
 * Moving a lane line on the phone keeps its angle: both ends shift by how far the handle moved. A level line stays
 * level (its right end stays 0).
 */
function moveLane(c, g, dir, fy, k) {
  const key = dir === 'R2L' ? 'R2LStreetY' : 'L2RStreetY', rkey = `${key}Right`;
  const yl = dir === 'R2L' ? g.r2l : g.l2r, yr = dir === 'R2L' ? g.r2lR : g.l2rR;
  const atHandle = laneYAt(g, yl, yr, laneHandleX(g, dir, k));
  const left = c[key], right = c[rkey] || 0;
  // The shift that keeps both ends inside the box.
  const lo = 1 - Math.min(left, right || left), hi = g.box.h - Math.max(left, right || left);
  const d = Math.round(Math.max(lo, Math.min(hi, fy - atHandle)));
  return right ? { [key]: left + d, [rkey]: right + d } : { [key]: left + d };
}

/** Inverse of geometry() for one dragged handle: returns the config keys it changes. */
export function applyHandle(c, handle, fx, fy, k = 2) {
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
        ...railZone({ AnalysisBoxWidth: right - nx, speedLineLeft: c.speedLineLeft - dx, speedLineRight: c.speedLineRight - dx }, true),
        R2LStreetY: clamp(c.R2LStreetY - dy, 0, bottom - ny), L2RStreetY: clamp(c.L2RStreetY - dy, 0, bottom - ny),
        ...(c.R2LStreetYRight ? { R2LStreetYRight: clamp(c.R2LStreetYRight - dy, 1, bottom - ny) } : {}),
        ...(c.L2RStreetYRight ? { L2RStreetYRight: clamp(c.L2RStreetYRight - dy, 1, bottom - ny) } : {}),
        ...(g.edge ? { streetEdge: c.streetEdge.map((e) => clamp(e - dy, 1, bottom - ny)) } : {}),
        obstruction: g.obs ? [Math.max(0, c.obstruction[0] - dx), Math.max(0, c.obstruction[1] - dx)] : c.obstruction,
      };
    }
    case 'br': {
      const w = clamp(fx, g.box.x + 200, W - 1) - g.box.x, h = clamp(fy, g.box.y + 60, H - 1) - g.box.y;
      return {
        AnalysisBoxWidth: w, AnalysisBoxHeight: h,
        ...railZone({ AnalysisBoxWidth: w, speedLineLeft: c.speedLineLeft, speedLineRight: c.speedLineRight }, false),
        R2LStreetY: Math.min(c.R2LStreetY, h), L2RStreetY: Math.min(c.L2RStreetY, h),
        ...(c.R2LStreetYRight ? { R2LStreetYRight: Math.min(c.R2LStreetYRight, h) } : {}),
        ...(c.L2RStreetYRight ? { L2RStreetYRight: Math.min(c.L2RStreetYRight, h) } : {}),
        ...(g.edge ? { streetEdge: c.streetEdge.map((e) => Math.min(e, h)) } : {}),
      };
    }
    // A speed line dragged out past the box's side takes the side with it, up to the picture's edge (as in VST
    // Studio), so the zone can go where the road is without resizing the box first. Then the rails (railZone).
    case 'sl': {
      const sr = c.speedLineRight;
      if (fx >= g.box.x) return railZone({ ...c, speedLineLeft: Math.min(fx - g.box.x, sr - MIN_ZONE) }, true);
      const nx = clamp(fx, 0, g.box.x), dx = nx - g.box.x;
      const grown = { AnalysisBoxLeft: nx, AnalysisBoxWidth: right - nx };
      return { ...grown, ...railZone({ ...grown, speedLineLeft: 0, speedLineRight: sr - dx }, true),
        obstruction: g.obs ? [c.obstruction[0] - dx, c.obstruction[1] - dx] : c.obstruction };
    }
    case 'sr': {
      const sl = c.speedLineLeft;
      if (fx <= right) return railZone({ ...c, speedLineRight: Math.max(fx - g.box.x, sl + MIN_ZONE) }, false);
      const w = clamp(fx, right, W - 1) - g.box.x;
      return { AnalysisBoxWidth: w, ...railZone({ AnalysisBoxWidth: w, speedLineLeft: sl, speedLineRight: w }, false) };
    }
    case 'r2l': return moveLane(c, g, 'R2L', fy, k);
    case 'l2r': return moveLane(c, g, 'L2R', fy, k);
    case 'o0': return { obstruction: [clamp(fx - g.box.x, 0, c.obstruction[1] - 10), c.obstruction[1]] };
    case 'o1': return { obstruction: [c.obstruction[0], clamp(fx - g.box.x, c.obstruction[0] + 10, g.box.w)] };
    default: return {};
  }
}

/**
 * Where handle `h` is drawn, in frame coordinates (the same spots zonesSvg puts them). The phone's nudge pad and
 * offset drags start from here and feed the moved point back through applyHandle.
 */
export function handlePoint(c, h, k = 2) {
  const g = geometry(c), { x, y, w, h: bh } = g.box;
  switch (h) {
    case 'tl': return [x, y];
    case 'br': return [x + w, y + bh];
    case 'sl': return [g.sl, y + bh / 2];
    case 'sr': return [g.sr, y + bh / 2];
    case 'r2l': case 'l2r': {
      const dir = h.toUpperCase(), hx = laneHandleX(g, dir, k);
      return [hx, laneYAt(g, dir === 'R2L' ? g.r2l : g.l2r, dir === 'R2L' ? g.r2lR : g.l2rR, hx)];
    }
    case 'o0': return g.obs ? [g.obs[0], y + bh - 16 * k] : null;
    case 'o1': return g.obs ? [g.obs[1], y + bh - 16 * k] : null;
    default: return null;
  }
}

/** Which way each handle can move: speed lines and obstruction edges slide sideways, lane lines up and down. */
/** One-way: the wrong-way direction's lane follows the legal one (App.cpp setLane(..., both)), since it is one lane. */
export function oneLane(c) {
  if (c.legalDirection !== 'L2R' && c.legalDirection !== 'R2L') return c;
  const from = c.legalDirection, to = from === 'L2R' ? 'R2L' : 'L2R';
  return { ...c, [`${to}StreetY`]: c[`${from}StreetY`], [`${to}StreetYRight`]: c[`${from}StreetYRight`] || 0 };
}

export const HANDLE_AXES = { tl: 'xy', br: 'xy', sl: 'x', sr: 'x', r2l: 'y', l2r: 'y', o0: 'x', o1: 'x' };

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
 *   selected - the handle id picked on the phone, drawn bigger with an accent ring so it is clear what the arrows move;
 *   tracks   - live tracked boxes [{x,y,w,h,dir,speed}], coloured by speed against the limit.
 * A one-way street (legalDirection L2R or R2L) has one lane line, like VST Studio's Mark your street: only the legal
 * direction's line and handle are drawn, and Setup keeps the other direction's line on top of it (oneLane()).
 */
export function zonesSvg(c, { k = 2, editable = false, selected = null, tracks = [], speedLimit = 25, egregious = 40 } = {}) {
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

  // The street edge (set in VST Studio): a thin green dashed line along the curb.
  if (g.edge) s += `<line x1="${x}" y1="${g.edge[0]}" x2="${x + w}" y2="${g.edge[1]}" stroke="${EDGE}" stroke-width="${sw * 0.8}" stroke-dasharray="${5 * k} ${4 * k}"/>`;

  // Lane ("hubcap") lines, at their angle.  One-way: just the one lane.
  const lanes = legal === 'L2R' || legal === 'R2L' ? [legal] : ['R2L', 'L2R'];
  for (const dir of lanes) {
    const ly = dir === 'R2L' ? g.r2l : g.l2r, lyR = dir === 'R2L' ? g.r2lR : g.l2rR;
    s += `<line x1="${x}" y1="${ly}" x2="${x + w}" y2="${lyR}" stroke="${LANE[dir]}" stroke-width="${sw}" stroke-dasharray="${2 * k} ${3 * k}"/>`;
    // Labels go on opposite sides of the two lines, which usually sit close together.
    const ty = laneYAt(g, ly, lyR, x + 6 * k);
    if (editable) s += label(x + 6 * k, dir === 'R2L' ? ty - 6 * k : ty + 15 * k, lanes.length === 1 ? 'LANE' : dir === 'R2L' ? '← WHEELS' : 'WHEELS →', LANE[dir]);
  }

  s += tracksSvg(tracks, { k, speedLimit, egregious });

  if (editable) {
    const hit = 24 * k;
    const handle = (id, hx, hy, col = '#fff', shape = 'circle') => {
      const on = id === selected, r = (on ? 11 : 9) * k;
      return `<g class="handle${on ? ' selected' : ''}" data-h="${id}">
      <circle cx="${hx}" cy="${hy}" r="${hit}" fill="transparent"/>
      ${on ? `<circle cx="${hx}" cy="${hy}" r="${r * 1.9}" fill="none" stroke="#5eead4" stroke-width="${2.5 * k}"/>` : ''}
      ${shape === 'circle'
        ? `<circle cx="${hx}" cy="${hy}" r="${r}" fill="${col}" stroke="#0b0d10" stroke-width="${2 * k}"/>`
        : `<rect x="${hx - r * 0.7}" y="${hy - r * 1.3}" width="${r * 1.4}" height="${r * 2.6}" rx="${r * 0.6}" fill="${col}" stroke="#0b0d10" stroke-width="${2 * k}"/>`}</g>`;
    };
    s += handle('tl', x, y);
    s += handle('br', x + w, y + h);
    s += handle('sl', g.sl, y + h / 2, '#fff', 'pill');
    s += handle('sr', g.sr, y + h / 2, '#fff', 'pill');
    // Lane handles sit at the right end, away from the box corner and the labels on the left.
    for (const dir of lanes) {
      const hx = laneHandleX(g, dir, k);
      s += handle(dir.toLowerCase(), hx, laneYAt(g, dir === 'R2L' ? g.r2l : g.l2r, dir === 'R2L' ? g.r2lR : g.l2rR, hx), LANE[dir]);
    }
    if (g.obs) { s += handle('o0', g.obs[0], y + h - 16 * k, '#cbd5e1', 'pill'); s += handle('o1', g.obs[1], y + h - 16 * k, '#cbd5e1', 'pill'); }
  }
  return s;
}
