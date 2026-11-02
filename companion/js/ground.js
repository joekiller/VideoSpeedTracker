/**
 * The demo's copy of VST Studio's ground-plane camera model for the parking lane (Perspective.h hubcapDepthAtRow and
 * laneScale, ParkingLane.h placeCar), so demo mode and the mock answer POST /api/calibration/car the way Studio does.
 * A real VST does this arithmetic itself; the companion only shows what it answers.  helpers.test.mjs checks these
 * against numbers printed by the C++.
 *
 * Camera coordinates as in Perspective.h: X along the street (+ right), Y down from the camera (the road at Y = height),
 * Z out from the camera's wall (the lane's middle at Z = distanceFt).  The camera is aimed at the lane (pitch from height
 * and distance) and turned yawDeg along the street; the picture is 1280 x 720 with its centre on the lens's axis.
 */
const D2R = Math.PI / 180;
export const CAR = { widthFt: 6, hubcapFt: 1.1 };
export const TYPICAL_PARK_FT = 7;

function rig(c, w = 1280, h = 720) {
  if (!(c.distanceFt > 0 && c.heightFt >= 0 && Math.abs(c.yawDeg) <= 75 && c.hfovDeg >= 10 && c.hfovDeg <= 170)) return null;
  const yaw = c.yawDeg * D2R, pitch = Math.atan(c.heightFt * Math.cos(yaw) / c.distanceFt);
  return { cy: Math.cos(yaw), sy: Math.sin(yaw), cp: Math.cos(pitch), sp: Math.sin(pitch), f: w / 2 / Math.tan(c.hfovDeg * D2R / 2), cx: w / 2, cyPix: h / 2 };
}

/** Where (X, Y, Z) lands in the picture, or null behind the camera / for a bad setup. */
export function projectPoint(c, X, Y, Z) {
  const r = rig(c);
  if (!r) return null;
  const x1 = r.cy * X - r.sy * Z, z1 = r.sy * X + r.cy * Z;
  const y2 = r.cp * Y - r.sp * z1, z2 = r.sp * Y + r.cp * z1;
  if (z2 <= 1e-6) return null;
  return { u: r.cx + r.f * x1 / z2, v: r.cyPix + r.f * y2 / z2 };
}

/** The X on the street-parallel line (Y, Z) that lands in column u; NaN where none does. */
export function streetXAt(c, u, Y, Z) {
  const r = rig(c);
  if (!r) return NaN;
  const k = u - r.cx, den = k * r.cp * r.sy - r.f * r.cy;
  if (Math.abs(den) < 1e-9) return NaN;
  const X = (-r.f * r.sy * Z - k * (r.sp * Y + r.cp * r.cy * Z)) / den;
  return projectPoint(c, X, Y, Z) ? X : NaN;
}

/** The lane's hubcap line: { Y, Z } of a moving car's near side, or null when the lane is too near. */
export function hubcapLine(c) {
  const Y = c.heightFt - CAR.hubcapFt, Z = c.distanceFt - CAR.widthFt / 2;
  return Z > 0.5 ? { Y, Z } : null;
}

const feetPerColumn = (c, u, Y, Z) => Math.abs(streetXAt(c, u + 0.5, Y, Z) - streetXAt(c, u - 0.5, Y, Z));

/** Camera Z of a hubcap-height line crossing column u at row v, the lane's crossing it at vLane; NaN if it can't say. */
export function hubcapDepthAtRow(c, u, vLane, v) {
  const h = hubcapLine(c);
  if (!rig(c) || !h || h.Y < 0.25) return NaN;
  const rowAt = (Z) => { const X = streetXAt(c, u, h.Y, Z); const p = Number.isNaN(X) ? null : projectPoint(c, X, h.Y, Z); return p ? p.v : NaN; };
  const model = rowAt(h.Z);
  if (Number.isNaN(model)) return NaN;
  const target = v - (vLane - model);
  let lo = 0.5, hi = Math.max(h.Z, 1) * 30;
  const vLo = rowAt(lo), vHi = rowAt(hi);
  if (Number.isNaN(vLo) || Number.isNaN(vHi) || target > vLo || target < vHi) return NaN;
  for (let i = 0; i < 80; i++) {
    const mid = (lo + hi) / 2, vm = rowAt(mid);
    if (Number.isNaN(vm)) return NaN;
    if (vm > target) lo = mid; else hi = mid;
  }
  return (lo + hi) / 2;
}

/** Span multiplier from depth Z to the lane at column u (> 1 farther). NaN where it can't be placed. */
export function laneScale(c, u, Z) {
  const h = hubcapLine(c);
  if (!h || !(Z > 0)) return NaN;
  const atLane = feetPerColumn(c, u, h.Y, h.Z);
  return atLane > 0 ? feetPerColumn(c, u, h.Y, Z) / atLane : NaN;
}

/** ParkingLane.h scaleBeyond: the multiplier for a car `beyondFt` beyond the moving cars (negative: nearer). */
export function scaleBeyond(c, u, beyondFt) {
  if (!beyondFt) return 1;
  const h = hubcapLine(c);
  if (!h || h.Z + beyondFt <= 0.5) return 1;
  const s = laneScale(c, u, h.Z + beyondFt);
  return s > 0 && Number.isFinite(s) ? s : (h.Z + beyondFt) / h.Z;
}

/**
 * ParkingLane.h placeCar: park = { y: [left, right] rows at x 0 and 1280, ft: measured (0 = from the picture) }.
 * Returns { where: 'parked'|'lane'|null, offLines, parkedFt, how: 'picture'|'measured'|'typical'|null, parkedScale }.
 */
export function placeCar(c, laneRow, park, midX, carRow) {
  const out = { where: null, offLines: false, parkedFt: NaN, how: null, parkedScale: 1 };
  if (!park || !(park.y[0] > 0 && park.y[1] > 0)) return out;
  const parkRow = park.y[0] + (park.y[1] - park.y[0]) * midX / 1280;
  const toLane = Math.abs(carRow - laneRow), toPark = Math.abs(carRow - parkRow), gap = Math.abs(parkRow - laneRow);
  out.where = toPark < toLane ? 'parked' : 'lane';
  out.offLines = Math.min(toLane, toPark) > Math.max(gap * 0.6, 30);
  Object.assign(out, parkedBeyond(c, midX, laneRow, parkRow, park.ft || 0));
  out.parkedScale = scaleBeyond(c, midX, out.parkedFt);
  return out;
}

/** ParkingLane.h parkedBeyond: how far beyond the lane the parking line is, signed, and how that was found. */
export function parkedBeyond(c, u, laneRow, parkRow, measuredFt) {
  const side = parkRow <= laneRow ? 1 : -1;
  if (measuredFt > 0) return { parkedFt: side * measuredFt, how: 'measured' };
  const h = hubcapLine(c), Z = hubcapDepthAtRow(c, u, laneRow, parkRow);
  if (h && Number.isFinite(Z)) return { parkedFt: Z - h.Z, how: 'picture' };
  return { parkedFt: side * TYPICAL_PARK_FT, how: 'typical' };
}
