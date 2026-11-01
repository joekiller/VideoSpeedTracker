// Speed trend: is a car slowing down, holding its speed or speeding up as it crosses the speed zone, and how fast will it
// be going a little further on?
//
// The tracker's speed is the average over the zone (a straight line through the front bumper's positions against time,
// VehicleDynamics::subFrameSpeed()).  Here a parabola goes through the same positions instead, plus the ones seen before
// the zone in the analysis box, which lengthens the baseline; its curvature is the car's acceleration, taken as constant
// over the half second or so a car is in view.
//
// Flow:  speedTrend(seconds, feet, zone, noise)
//          -> least-squares fit  feet = b0 + b1*tau + b2*tau^2  (tau = seconds - their mean, which keeps it well conditioned)
//          -> acceleration a = 2*b2, its standard error from the fit's covariance and the bumper jitter, which is the
//             residuals pooled with kPriorDof degrees of freedom' worth of the caller's noise floor (a pixel, in feet):
//             a car seen 5-7 times, as in a 20 ft view at 30 fps, has only 2-4 residual degrees of freedom of its own
//          -> speed at the entry line (0 ft) and the exit line (zone ft) from v^2 = v0^2 + 2*a*(x - x0), the constant-
//             acceleration relation, so the two speeds and a always agree
//          -> classifyTrend(a, SE, k) and project the exit speed kProjectFeet further on.
//
// Classes (all in mph per second, 1 mph/s = 1.47 ft/s^2 = 0.046 g):
//   Decelerating          a <= -2   braking or easing off; a car rolling to a stop sign brakes at 5-10
//   Steady                |a| < 2   cruising; 2 mph/s over a 20 ft zone at 20 mph changes the speed by only 1.4 mph
//   Accelerating          2 <= a < 6   pulling away normally
//   RapidlyAccelerating   a >= 6   flooring it: harder than most drivers ever accelerate (0-60 in 10 s is 6 mph/s);
//                                  insurers' "harsh acceleration" is about 7-8
// and two rules for noise, because a 14-20 ft zone sees a car for only 6-16 frame pairs:
//   - a class other than Steady needs |a| >= k SE, where k is Student's t for 98% two-sided with n - 3 + kPriorDof
//     degrees of freedom (3.0 for 6 points, 2.6 for 15): a steady car is called accelerating or decelerating in about 2%
//     of passes.  With a plain 2 SE, 14 ft zones (8-9 points) gave 5-8% false calls in the simulations;
//   - when k SE is more than kRapid (the fit can't tell steady from rapid), the class is Unknown unless |a| clears it.
// Unknown is also given for fewer than kMinPoints positions, a fit that strays more than maxRmsFt (two cars merged, a
// track picked up again), or a car that isn't moving forward in the fit.
//
// Anchoring: the tracker's own speed (the zone average from subFrameSpeed() or whole frames) is the number VST reports,
// so when the caller passes it as anchorMph the entry and exit speeds are shifted together until their average is that
// speed (for constant acceleration the average speed over a stretch of road is exactly the mean of its end speeds).
// The difference between them, the acceleration and the class come from the fit alone; a table never reads
// "25 mph: came in at 27, left at 28".
//
// Projection: the exit speed carried kProjectFeet (20 ft) further at the measured acceleration, sqrt(v^2 + 2 a d), 0 if it
// would stop first.  A Steady car is projected at its exit speed (acceleration taken as 0), so the projection never
// shows a change the class says can't be measured.  Unknown has no projection.
//
// It knows nothing about OpenCV or the tracker, so tests/speed_trend_test.cpp drives it with made-up tracks.
#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <vector>

namespace vst {

enum class Trend { Unknown = 0, Decelerating, Steady, Accelerating, RapidlyAccelerating };

constexpr double kFtPerSecPerMph = 5280.0 / 3600.0;
constexpr double kSteadyMphPerSec = 2.0;   // |a| below this is Steady
constexpr double kRapidMphPerSec = 6.0;    // a at or above this is RapidlyAccelerating
constexpr double kProjectFeet = 20.0;      // how far past the exit line the projection looks
constexpr int kMinPoints = 5;              // front bumpers needed for a parabola with residuals to judge it by
constexpr double kPriorDof = 4;            // the noise floor counts as this many residual degrees of freedom

struct SpeedTrend {
	Trend trend = Trend::Unknown;
	double entryMph = 0, exitMph = 0;      // at the entry and exit speed lines, from the fit
	double accelMphPerSec = 0;             // + speeding up, - slowing down
	double accelSE = 0;                    // its standard error, mph/s
	double projectedMph = 0;               // kProjectFeet past the exit line (0 when Unknown)
	int points = 0;                        // front bumpers the fit used
	double rmsFt = 0;                      // how far they stray from the parabola, rms
	bool known() const { return trend != Trend::Unknown; }
};

// Csv and API names: "decelerating", "steady", "accelerating", "rapid"; "" for Unknown.
inline const char* trendName(Trend t) {
	switch (t) {
	case Trend::Decelerating: return "decelerating";
	case Trend::Steady: return "steady";
	case Trend::Accelerating: return "accelerating";
	case Trend::RapidlyAccelerating: return "rapid";
	default: return "";
	}
}
// The inverse, for reading a csv back; anything else is Unknown.
inline Trend trendFromName(const char* s) {
	for (Trend t : { Trend::Decelerating, Trend::Steady, Trend::Accelerating, Trend::RapidlyAccelerating }) {
		const char* n = trendName(t);
		int i = 0;
		while (n[i] && s[i] == n[i]) i++;
		if (!n[i] && !s[i]) return t;
	}
	return Trend::Unknown;
}
// For people: "Slowing down", "Steady", "Speeding up", "Rapidly speeding up", "" for Unknown.
inline const char* trendLabel(Trend t) {
	switch (t) {
	case Trend::Decelerating: return "Slowing down";
	case Trend::Steady: return "Steady";
	case Trend::Accelerating: return "Speeding up";
	case Trend::RapidlyAccelerating: return "Rapidly speeding up";
	default: return "";
	}
}

// Student's t, 98% two-sided (0.99 quantile), for dof degrees of freedom: how many standard errors an acceleration must
// clear before it counts.  Linear between the tabulated values, 2.33 (the normal's) past 120.
inline double tCritical98(int dof) {
	static const int d[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 15, 20, 30, 60, 120 };
	static const double t[] = { 31.82, 6.965, 4.541, 3.747, 3.365, 3.143, 2.998, 2.896, 2.821, 2.764, 2.681, 2.602, 2.528, 2.457, 2.390, 2.358 };
	if (dof < 1) return t[0];
	for (int i = 1; i < 16; i++)
		if (dof <= d[i]) return t[i - 1] + (t[i] - t[i - 1]) * double(dof - d[i - 1]) / double(d[i] - d[i - 1]);
	return 2.326;
}

// The class for an acceleration a with standard error se (both mph/s), which must clear k standard errors to count
// (tCritical98), by the rules at the top of this file.
inline Trend classifyTrend(double a, double se, double k) {
	bool clear = std::fabs(a) >= k * se;   // distinguishable from 0
	if (!clear) return k * se > kRapidMphPerSec ? Trend::Unknown : Trend::Steady;
	if (a <= -kSteadyMphPerSec) return Trend::Decelerating;
	if (a >= kRapidMphPerSec) return Trend::RapidlyAccelerating;
	if (a >= kSteadyMphPerSec) return Trend::Accelerating;
	return Trend::Steady;
}

// seconds[i], feet[i]: when each front bumper was seen, and how far along the direction of travel it was from the entry
// line (negative before it, zoneFeet at the exit line).  noiseFt: the least position noise to assume (bumper jitter of a
// few pixels, in feet), so a lucky near-perfect fit on few points doesn't claim more than the picture can show.
// maxRmsFt: a fit straying further than this on average isn't one car moving smoothly.
// anchorMph: the speed VST reported for this pass (see Anchoring above), or 0 to use the fit's own speeds.
inline SpeedTrend speedTrend(const std::vector<double>& seconds, const std::vector<double>& feet, double zoneFeet,
	double noiseFt, double maxRmsFt, double anchorMph = 0) {
	SpeedTrend r;
	const size_t n = std::min(seconds.size(), feet.size());
	r.points = int(n);
	if (n < size_t(kMinPoints) || zoneFeet <= 0) return r;

	// Normal equations for feet = b0 + b1 tau + b2 tau^2.  S[k] = sum tau^k, T[k] = sum feet * tau^k.
	double mean = 0;
	for (size_t i = 0; i < n; i++) mean += seconds[i];
	mean /= double(n);
	double S[5] = { 0, 0, 0, 0, 0 }, T[3] = { 0, 0, 0 };
	for (size_t i = 0; i < n; i++) {
		double tau = seconds[i] - mean, p = 1;
		for (int k = 0; k < 5; k++) { S[k] += p; if (k < 3) T[k] += feet[i] * p; p *= tau; }
	}
	// M = [[S0 S1 S2] [S1 S2 S3] [S2 S3 S4]]; its inverse by cofactors (symmetric).
	const double m00 = S[0], m01 = S[1], m02 = S[2], m11 = S[2], m12 = S[3], m22 = S[4];
	const double c00 = m11 * m22 - m12 * m12, c01 = -(m01 * m22 - m12 * m02), c02 = m01 * m12 - m11 * m02;
	const double c11 = m00 * m22 - m02 * m02, c12 = -(m00 * m12 - m01 * m02), c22 = m00 * m11 - m01 * m01;
	const double det = m00 * c00 + m01 * c01 + m02 * c02;
	if (!(std::fabs(det) > 1e-12)) return r;   // all at (nearly) the same time
	const double b0 = (c00 * T[0] + c01 * T[1] + c02 * T[2]) / det;
	const double b1 = (c01 * T[0] + c11 * T[1] + c12 * T[2]) / det;
	const double b2 = (c02 * T[0] + c12 * T[1] + c22 * T[2]) / det;

	double ssr = 0;
	for (size_t i = 0; i < n; i++) {
		double tau = seconds[i] - mean;
		ssr += std::pow(feet[i] - (b0 + b1 * tau + b2 * tau * tau), 2);
	}
	r.rmsFt = std::sqrt(ssr / double(n));
	if (r.rmsFt > maxRmsFt) return r;
	if (b1 <= 0) return r;   // not moving forward at the middle of the track

	// Var(b2) = sigma^2 * (M^-1)[2][2].  sigma^2 pools the residuals (n - 3 degrees of freedom) with kPriorDof of the
	// noise floor, so 2 lucky residuals can't claim a perfect track and a ragged one still shows its own spread.
	const double dof = double(n - 3) + kPriorDof;
	const double sigma2 = (ssr + kPriorDof * noiseFt * noiseFt) / dof;
	const double a = 2 * b2;                                        // ft/s^2
	const double se = 2 * std::sqrt(sigma2 * std::max(c22 / det, 0.0));
	r.accelMphPerSec = a / kFtPerSecPerMph;
	r.accelSE = se / kFtPerSecPerMph;

	// Constant acceleration: v^2 grows by 2 a per foot travelled.  Reference: the middle of the track (tau = 0).
	auto speedAt = [&](double x) { return std::sqrt(std::max(b1 * b1 + 2 * a * (x - b0), 0.0)); };   // ft/s
	r.entryMph = speedAt(0) / kFtPerSecPerMph;
	r.exitMph = speedAt(zoneFeet) / kFtPerSecPerMph;
	if (anchorMph > 0) {
		const double shift = anchorMph - (r.entryMph + r.exitMph) / 2;
		r.entryMph = std::max(r.entryMph + shift, 0.0);
		r.exitMph = std::max(r.exitMph + shift, 0.0);
	}
	r.trend = classifyTrend(r.accelMphPerSec, r.accelSE, tCritical98(int(dof)));
	if (r.trend == Trend::Unknown) return r;
	const double vExit = r.exitMph * kFtPerSecPerMph;
	if (r.trend == Trend::Steady) r.projectedMph = r.exitMph;
	else r.projectedMph = std::sqrt(std::max(vExit * vExit + 2 * a * kProjectFeet, 0.0)) / kFtPerSecPerMph;
	return r;
}

}  // namespace vst
