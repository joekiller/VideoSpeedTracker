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

export const SOURCE_NAMES = { guess: 'Rough guess', typed: 'Your numbers', picture: 'From your picture', calibration: 'Measured' };
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
  return { cover, zone, frames: at.frames, samples: at.samples, quant, calib, total: Math.hypot(quant, calib) };
}

export const fastestShortMph = (zoneM, fps) => zoneM * fps / (2 * (MIN_SAMPLES - 1)) / MPH;
export const fastestText = (mph) => (mph > MAX_TRACK_MPH ? `over ${MAX_TRACK_MPH} mph` : `up to about ${Math.floor(mph / 5) * 5} mph`);

/** plan::verdict and plan::verdictText. */
export function verdict(p, shortTimingOn) {
  const checkMph = p.speedLimit + 10;
  const whole = evaluate(p, checkMph, false);
  const shortView = shortTimingOn || whole.zone <= 0 || whole.frames < SHORT_FRAMES;
  const at = shortView ? evaluate(p, checkMph, true) : whole;
  const tooFew = shortView && at.samples < MIN_SAMPLES;
  const grade = at.zone <= 0 || tooFew ? 2 : at.total <= 1.5 ? 0 : at.total <= 3 ? 1 : 2;
  const zoneFt = at.zone * FT;
  const text = at.zone <= 0
    ? 'Only a couple of feet of street fit in the picture. Set the camera further back or zoom out, and it will fit.'
    : tooFew
      ? `A car at ${checkMph} mph crosses the ${zoneFt.toFixed(0)} ft zone in ${at.frames.toFixed(0)} frames. Slower cars still time fine; a 60 fps camera or a little more street times this one too.`
      : `About ±${at.total.toFixed(1)} mph at ${checkMph} mph, over a ${zoneFt.toFixed(0)} ft zone.`;
  const short = shortView && at.zone > 0;
  return {
    grade, label: GRADES[grade], text, checkMph, shortView,
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
