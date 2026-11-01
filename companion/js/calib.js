/**
 * Combining calibration measurements, for the demo (VST Studio's src/Gui/CalEvidence.h/.cpp does it for real and
 * reports the result in GET /api/status `calibration`).
 *
 * Every method says how many feet of street lie between the speed lines, give or take a relative `sigma`. The
 * calibration is their inverse-variance weighted mean (w = 1/sigma²), its sigma 1/sqrt(Σw) widened by the Birge ratio
 * sqrt(χ²/(n-1)) when they disagree, and with three or more an input that is off the others by 3 sigma and 4% is
 * flagged. Checked against the same numbers as tests/calibration_test.cpp in companion/test/helpers.test.mjs.
 */

export const FT_PER_S_PER_MPH = 5280 / 3600;
export const OUTLIER_PCT = 4;

export const METHOD_NAMES = {
  pass: 'Drive past', check: 'Check drive', cones: 'Cones or pole', car: 'Known car', camera: 'Camera setup', earlier: 'Earlier calibration',
};

export const zoneFtFromPass = (mph, frames, fps) => (mph > 0 && frames > 0 && fps > 0 ? mph * FT_PER_S_PER_MPH * frames / fps : 0);
export const framesForZone = (zoneFt, calSpeed, calFps) => (zoneFt > 0 && calSpeed > 0 && calFps > 0 ? zoneFt / (calSpeed * FT_PER_S_PER_MPH) * calFps : 0);
/** A pass: speedometer ~2% plus 0.5 mph, and whole-frame counting (sqrt(2/12) ≈ 0.41 frame). */
export const sigmaPass = (mph, frames) => (mph > 0 && frames > 0 ? Math.hypot(Math.hypot(0.02 * mph, 0.5) / mph, 0.41 / frames) : 1);
/** Two marks on a tape-measured spacing: 1% tape, ~4 px of clicking over the span, 2.5% for the marks' depth. */
export const sigmaCones = (markPx) => (markPx > 0 ? Math.sqrt(0.01 ** 2 + (4 / markPx) ** 2 + 0.025 ** 2) : 1);

const applies = (input, dir) => input.dirs === 'both' || input.dirs === dir;

/** What the inputs add up to for `dir` ('L2R' | 'R2L') on speed lines `lines`. */
export function combine(inputs, dir, lines) {
  const c = { valid: false, n: 0, zoneFt: 0, sigma: 0, spreadPct: 0, birge: 1, shares: [], outliers: 0, setAside: 0, agree: false };
  const use = [];
  for (const i of inputs) {
    if (i.removed || !applies(i, dir) || !(i.zoneFt > 0) || !(i.sigma > 0)) continue;
    if (i.lines && lines && i.lines !== lines) { c.setAside++; continue; }
    use.push(i);
  }
  c.n = use.length;
  if (!c.n) return c;
  const w = use.map((i) => 1 / (i.sigma * i.sigma));
  const sw = w.reduce((a, b) => a + b, 0);
  const swx = use.reduce((a, i, k) => a + w[k] * i.zoneFt, 0);
  c.valid = true;
  c.zoneFt = swx / sw;
  const chi2 = use.reduce((a, i, k) => a + w[k] * (i.zoneFt / c.zoneFt - 1) ** 2, 0);
  c.birge = c.n > 1 ? Math.sqrt(chi2 / (c.n - 1)) : 1;
  c.sigma = 1 / Math.sqrt(sw) * Math.max(1, c.birge);
  // "Agree within X%": the range of the inputs, as a share of the result.
  c.spreadPct = (Math.max(...use.map((i) => i.zoneFt)) - Math.min(...use.map((i) => i.zoneFt))) / c.zoneFt * 100;
  use.forEach((i, k) => {
    const s = { id: i.id, contribution: w[k] / sw, offPct: (i.zoneFt / c.zoneFt - 1) * 100, outlier: false };
    if (c.n >= 3) {
      // Off the others by 3 sigma and 4%, and the others agree among themselves (else no single one is the odd one out).
      const ow = sw - w[k], ox = (swx - w[k] * i.zoneFt) / ow;
      const dev = Math.abs(i.zoneFt / ox - 1);
      let ochi2 = 0, ospread = 0;
      use.forEach((j, m) => { if (m !== k) { const r = j.zoneFt / ox - 1; ochi2 += w[m] * r * r; ospread = Math.max(ospread, Math.abs(r)); } });
      const othersAgree = ospread * 100 <= 3 + 1e-9 || Math.sqrt(ochi2 / (c.n - 2)) <= 1.5;
      s.outlier = othersAgree && dev > 3 * Math.hypot(i.sigma, 1 / Math.sqrt(ow)) && dev * 100 > OUTLIER_PCT;
      c.outliers += s.outlier;
    }
    c.shares.push(s);
  });
  c.agree = c.n >= 2 && c.outliers === 0 && (c.spreadPct <= 3 + 1e-9 || (c.birge <= 1.5 && c.spreadPct <= 6 + 1e-9));
  return c;
}

/** The novice line and the next step, in the same words as VST Studio (calib::summary). */
export function summary(c) {
  if (!c.valid) {
    return c.setAside
      ? { summary: "Your earlier measurements don't fit these speed lines", detail: 'They were made with the speed lines somewhere else. Measure again on these lines; one drive past is enough.' }
      : { summary: 'Not calibrated yet', detail: '' };
  }
  const within = Math.max(1, Math.ceil(c.spreadPct - 1e-9));
  if (c.n === 1) return { summary: '1 measurement so far', detail: 'Add another way (a drive past, cones, or a car you know) and VST cross-checks them.' };
  if (c.outliers) {
    return c.outliers === 1
      ? { summary: `${c.n} measurements, 1 well off the others`, detail: 'Take out the one marked if you doubt it, or add another measurement to settle it.' }
      : { summary: `${c.n} measurements, ${c.outliers} well off the rest`, detail: 'Take out the ones marked if you doubt them, or add another measurement to settle it.' };
  }
  if (c.agree) return { summary: `${c.n} measurements agree within ${within}%`, detail: '' };
  return {
    summary: `${c.n} measurements differ by ${within}%`,
    detail: c.n === 2 ? 'VST uses a weighted middle. A third measurement will show which one is off.' : 'VST uses a weighted middle. Remove one you doubt, or add another.',
  };
}

/** GET /api/status `calibration.<dir>.combined`, as VST Studio's Server.cpp shapes it. */
export function combinedJson(inputs, dir, lines) {
  const c = combine(inputs, dir, lines);
  const s = summary(c);
  return {
    summary: s.summary, detail: s.detail, n: c.n, agree: c.agree,
    zoneFt: c.valid ? Math.round(c.zoneFt * 100) / 100 : null,
    sigmaPct: c.valid ? Math.round(c.sigma * 1000) / 10 : null,
    setAside: c.setAside,
    shares: c.shares.map((x) => ({ id: x.id, contribution: Math.round(x.contribution * 1000) / 1000, offPct: Math.round(x.offPct * 10) / 10, outlier: x.outlier })),
  };
}
