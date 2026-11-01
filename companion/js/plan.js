/**
 * The site planner's model for the demo (VST Studio's src/Gui/Plan.cpp, which serves GET/PUT /api/plan for real).
 *
 * Only demo.js uses this: in live mode the phone shows whatever VST Studio's own planner says. It is kept to the
 * parts the phone shows (presets, measuring from car lengths or a calibration, the verdict and its words) and checked
 * against the numbers tests/plan_test.cpp checks, in companion/test/helpers.test.mjs, so the two can't drift far.
 *
 * Lengths are feet at this level (metres inside `evaluate`, like the C++).
 */

export const FT = 3.28084;
const MPH = 0.44704;
const LOCK_S = 0.2, EDGE_M = 0.3, JITTER_PX = 12, MIN_SAMPLES = 4, SHORT_FRAMES = 15, MAX_TRACK_MPH = 70, FRAME_PX = 1280;
export const TYPICAL_CAR_FT = 15.5;

export const PRESETS = [
  { key: 'narrow', name: 'Narrow street from a front window', distFt: 20, clearFt: 20,
    hint: 'An old-city or one-way street with parking, seen from a front window across a small yard: about 20 ft to the moving lane, and trees or porches leave about 20 ft of it clear.' },
  { key: 'neighborhood', name: 'Two-lane street from the house', distFt: 35, clearFt: 50,
    hint: 'A two-way neighborhood street seen from a front window or porch: about 35 ft to the nearer lane, 50 ft of it clear.' },
  { key: 'wide', name: 'Wide street or a long, open view', distFt: 50, clearFt: 90,
    hint: 'A wider street, or a view along a fence or a park with nothing in the way: about 50 ft to the lane, 90 ft clear.' },
];

export const SOURCE_NAMES = { guess: 'Rough guess', typed: 'Your numbers', picture: 'From your picture', calibration: 'Measured', map: 'From the map' };
const GRADES = ['Good to go', 'Works, a bit rougher', 'Needs a tweak'];

export const coverFt = (distFt, hfov) => 2 * distFt * Math.tan(hfov * Math.PI / 360);
export const distForCoverFt = (cover, hfov) => cover / (2 * Math.tan(Math.min(170, Math.max(5, hfov)) * Math.PI / 360));

/** plan::evaluate at one speed. `p` has distFt, clearFt, hfov, width, fps, shutter, carLenFt, speedLimit. */
export function evaluate(p, mph, shortView) {
  const v = mph * MPH, distM = p.distFt / FT, clearM = p.clearFt / FT, carM = (p.carLenFt ?? 15.75) / FT;
  const cover = 2 * distM * Math.tan(p.hfov * Math.PI / 360);
  const visible = Math.min(cover, clearM);
  const zone = shortView ? Math.max(0, visible - 2 * EDGE_M) : Math.max(0, visible - carM - v * LOCK_S);
  const timing = (s) => {
    const frames = zone / (s * MPH) * p.fps;
    const samples = zone > 0 ? frames / 2 + 1 : 0;
    let frac;
    if (!shortView) frac = frames > 0 ? 1 / frames : Infinity;
    else if (samples < MIN_SAMPLES) frac = Infinity;
    else frac = (JITTER_PX * cover / FRAME_PX) * Math.sqrt(12 / samples) / zone;
    return { frames, samples, frac };
  };
  const at = timing(mph);
  const quant = mph * at.frac;
  const calib = zone > 0 ? mph * timing(p.speedLimit).frac : Infinity;
  // Motion blur in picture pixels: the car moves v / shutter metres while the shutter is open.
  const blurPx = v / p.shutter * (p.width / Math.max(cover, .01));
  return { cover, zone, frames: at.frames, samples: at.samples, quant, calib, total: Math.hypot(quant, calib), blurPx };
}

export const fastestShortMph = (zoneM, fps) => zoneM * fps / (2 * (MIN_SAMPLES - 1)) / MPH;
export const fastestText = (mph) => (mph > MAX_TRACK_MPH ? `over ${MAX_TRACK_MPH} mph` : `up to about ${Math.floor(mph / 5) * 5} mph`);

/** plan::verdict and plan::verdictText. */
export function verdict(p, shortTimingOn) {
  const checkMph = p.speedLimit + 10;
  const whole = evaluate(p, checkMph, false);
  // Short-view timing also wins when it simply does better on a view whole-frame timing grades rougher, so a 60 fps
  // camera never grades worse than a 30 fps phone (plan::verdict's partBetter).
  const part = evaluate(p, checkMph, true);
  const partBetter = part.zone > 0 && part.samples >= MIN_SAMPLES && Number.isFinite(part.total)
    && (!Number.isFinite(whole.total) || (whole.total > 1.5 && part.total < whole.total));
  const shortView = shortTimingOn || whole.zone <= 0 || whole.frames < SHORT_FRAMES || partBetter;
  const at = shortView ? part : whole;
  const tooFew = shortView && at.samples < MIN_SAMPLES;
  let grade = at.zone <= 0 || tooFew ? 2 : at.total <= 1.5 ? 0 : at.total <= 3 ? 1 : 2;
  // Heavy blur smears the car's edges: never "Good to go" next to it.
  const blurry = at.blurPx > 25;
  if (blurry) grade = Math.max(grade, 1);
  const zoneFt = at.zone * FT;
  const text = at.zone <= 0
    ? 'Only a couple of feet of street fit in the picture. Set the camera further back or zoom out, and it will fit.'
    : tooFew
      ? `A car at ${checkMph} mph crosses the ${zoneFt.toFixed(0)} ft zone in ${at.frames.toFixed(0)} frames. Slower cars still time fine; a 60 fps camera or a little more street times this one too.`
      : `About ±${at.total.toFixed(1)} mph at ${checkMph} mph, over a ${zoneFt.toFixed(0)} ft zone.`;
  const said = blurry && at.zone > 0 ? `${text} Cars blur a lot at this shutter: 1/500 s or faster sharpens them.` : text;
  const short = shortView && at.zone > 0;
  return {
    grade, label: GRADES[grade], text: said, checkMph, shortView, blurry,
    errorMph: Number.isFinite(at.total) ? Math.round(at.total * 10) / 10 : null,
    zoneFt: Math.round(zoneFt * 10) / 10,
    fastest: short ? fastestText(fastestShortMph(at.zone, p.fps)) : null,
    fastest60: short ? fastestText(fastestShortMph(at.zone, 60)) : null,
  };
}

/** plan::fromCalibration, in feet. */
export function fromCalibration(calFrames, calSpeed, calFps, zonePx, boxWidthPx, obstructionPx) {
  if (!(calFrames > 0 && calSpeed > 0 && calFps > 0 && zonePx >= 10)) return null;
  const zoneFt = calSpeed * 5280 / 3600 * calFrames / calFps;
  const pxPerFt = zonePx / zoneFt;
  const cover = FRAME_PX / pxPerFt;
  const clear = Math.min(cover, Math.max(0, boxWidthPx - Math.max(0, obstructionPx)) / pxPerFt);
  return cover >= 3 && cover <= 1000 && clear > 0 ? { coverFt: cover, clearFt: clear, zoneFt } : null;
}

/** plan::fromCarLengths, in feet; boxWidthPx 0 = no box yet (the whole frame). */
export function fromCarLengths(n, boxWidthPx = 0, obstructionPx = 0) {
  if (!(n > 0)) return null;
  const cover = n * TYPICAL_CAR_FT;
  const pxPerFt = FRAME_PX / cover;
  return { coverFt: cover, clearFt: Math.min(cover, Math.max(0, (boxWidthPx || FRAME_PX) - Math.max(0, obstructionPx)) / pxPerFt) };
}

// ---- the map (plan::fromMap) ----------------------------------------------------------------------------------

const EARTH_M = 6371008.8;
const rad = (d) => d * Math.PI / 180;
const wrap360 = (d) => ((d % 360) + 360) % 360;
const angleBetween = (a, b) => { const d = Math.abs(wrap360(a) - wrap360(b)); return d > 180 ? 360 - d : d; };
export const COMPASS = ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'];

/** Great-circle distance in metres (haversine). */
export function groundDistanceM(lat1, lon1, lat2, lon2) {
  const dLat = rad(lat2 - lat1), dLon = rad(lon2 - lon1);
  const a = Math.sin(dLat / 2) ** 2 + Math.cos(rad(lat1)) * Math.cos(rad(lat2)) * Math.sin(dLon / 2) ** 2;
  return 2 * EARTH_M * Math.asin(Math.min(1, Math.sqrt(a)));
}
/** Initial bearing from point 1 to point 2, degrees clockwise from north, 0-360. */
export function bearingDeg(lat1, lon1, lat2, lon2) {
  const y = Math.sin(rad(lon2 - lon1)) * Math.cos(rad(lat2));
  const x = Math.cos(rad(lat1)) * Math.sin(rad(lat2)) - Math.sin(rad(lat1)) * Math.cos(rad(lat2)) * Math.cos(rad(lon2 - lon1));
  return wrap360(Math.atan2(y, x) * 180 / Math.PI);
}
export const compass8 = (d) => COMPASS[Math.floor(wrap360(d) / 45 + 0.5) % 8];

/**
 * The two pins (and the street OpenStreetMap has under the lane pin) as the planner's numbers: the distance, which
 * compass heading a car crossing the picture left to right has (the street's direction nearest the camera's
 * facing + 90), and the legal direction on a one-way.  null unless both pins are placed 2-500 ft apart.
 * `m`: { camLat, camLon, laneLat, laneLon, wayBearing?, oneway? (1 along wayBearing, -1 against, 0 two-way), maxspeedMph? }.
 */
export function fromMap(m) {
  if (!(m.camLat || m.camLon) || !(m.laneLat || m.laneLon)) return null;
  const distM = groundDistanceM(m.camLat, m.camLon, m.laneLat, m.laneLon);
  if (distM < 2 / FT || distM > 500 / FT) return null;
  const facing = bearingDeg(m.camLat, m.camLon, m.laneLat, m.laneLon);
  const right = facing + 90;
  const haveWay = typeof m.wayBearing === 'number';
  const l2rDeg = wrap360(!haveWay ? right : angleBetween(m.wayBearing, right) <= 90 ? m.wayBearing : m.wayBearing + 180);
  const l2r = compass8(l2rDeg);
  let r2l = compass8(l2rDeg + 180);
  if (r2l === l2r) r2l = compass8(l2rDeg + 225);
  let legal = 'both';
  if (haveWay && m.oneway) legal = angleBetween(m.oneway > 0 ? m.wayBearing : m.wayBearing + 180, l2rDeg) < 90 ? 'L2R' : 'R2L';
  const speedLimit = m.maxspeedMph >= 5 && m.maxspeedMph <= 80 ? m.maxspeedMph : 0;
  return { distFt: distM * FT, facing: compass8(facing), facingDeg: facing, l2rDeg, l2r, r2l, legal, speedLimit };
}
