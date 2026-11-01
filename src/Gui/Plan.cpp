// The site planner's model; see Plan.h.  The timing model itself is unchanged from the one Guide.cpp carried, and
// Guide.cpp's header explains where its constants come from.
#include "Plan.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace plan {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

double coverAt(double distM, double hfovDeg) {
	return 2 * distM * std::tan(hfovDeg * kPi / 180 / 2);
}

double distForCover(double coverM, double hfovDeg) {
	double t = std::tan(std::clamp(hfovDeg, 5.0, 170.0) * kPi / 180 / 2);
	return coverM / (2 * t);
}

Eval evaluate(const Inputs& p, double mph, bool shortView) {
	Eval e{};
	double v = mph * kMph;
	e.shortView = shortView;
	e.cover = coverAt(p.distM, p.hfovDeg);
	e.visible = std::min(e.cover, p.clearM);
	// Whole-frame timing needs a whole car in the zone plus a run-up to lock on; short-view timing only the bumper.
	e.zone = shortView ? std::max(0.0, e.visible - 2 * kEdgeM) : std::max(0.0, e.visible - p.carLenM - v * kLockTime);
	// Timing error of one crossing at s mph, as a fraction of its speed.
	auto timing = [&](double s, double* framesOut, double* samplesOut) -> double {
		double frames = e.zone / (s * kMph) * p.fps;
		// The engine looks at frames in pairs: one bumper position per pair, plus the one beyond the far line.
		double samples = e.zone > 0 ? frames / 2 + 1 : 0;
		if (framesOut) *framesOut = frames;
		if (samplesOut) *samplesOut = samples;
		if (!shortView) return frames > 0 ? 1 / frames : INFINITY;
		if (samples < kMinSamples) return INFINITY;
		// Slope error of a least-squares line through n evenly spaced points with position noise sigma over a run of
		// length L: sigma * sqrt(12 / n) / L, as a fraction of the speed.
		double jitterM = kJitterPx * e.cover / kFrameWidthPx;
		return jitterM * std::sqrt(12 / samples) / e.zone;
	};
	e.quant = mph * timing(mph, &e.frames, &e.samples);
	// VST never measures the zone: it learns it from a pass at a known speed (step 5), so the calibration carries that
	// pass's own timing error, here a drive past at the speed limit, into every speed.
	e.calib = e.zone > 0 ? mph * timing(p.speedLimit, nullptr, nullptr) : INFINITY;
	e.total = std::hypot(e.quant, e.calib);
	e.pxPerM = p.widthPx / std::max(e.cover, .01);
	e.blurCm = v / p.shutter * 100;
	e.blurPx = e.blurCm / 100 * e.pxPerM;
	return e;
}

double fastestShortMph(double zoneM, double fps) {
	return zoneM * fps / (2 * (kMinSamples - 1)) / kMph;
}

std::string fastestText(double mph) {
	char b[40];
	if (mph > kMaxTrackMph) std::snprintf(b, sizeof b, "over %.0f mph", kMaxTrackMph);
	else std::snprintf(b, sizeof b, "up to about %.0f mph", std::floor(mph / 5) * 5);
	return b;
}

Verdict verdict(const Inputs& in, bool shortTimingOn) {
	Verdict r;
	r.checkMph = in.speedLimit + 10;
	// A view too short for whole-frame timing is planned with short-view timing, whether or not it's on yet.
	Eval whole = evaluate(in, r.checkMph, false);
	// And when short-view timing would simply do better on a view whole-frame timing grades rougher: otherwise a 60 fps camera (enough whole frames, so planned the
	// old way) grades worse than a 30 fps phone planned with short-view timing, which reads as "don't upgrade".
	Eval part = evaluate(in, r.checkMph, true);
	// A view whole-frame timing already handles well (a good grade) keeps it.
	bool partBetter = part.zone > 0 && part.samples >= kMinSamples && std::isfinite(part.total)
		&& (!std::isfinite(whole.total) || (whole.total > 1.5 && part.total < whole.total));
	r.shortView = shortTimingOn || whole.zone <= 0 || whole.frames < kShortFrames || partBetter;
	r.at = r.shortView ? part : whole;
	r.tooFew = r.shortView && r.at.samples < kMinSamples;
	r.grade = r.at.zone <= 0 || r.tooFew ? 2 : grade(r.at.total, 1.5, 3);
	// Heavy motion blur (the planner's red tile) smears the car's edges, so the verdict can't be "Good to go" next to it;
	// it reads as rougher, and the sentence says what fixes it.
	r.blurry = r.at.blurPx > 25;
	if (r.blurry) r.grade = std::max(r.grade, 1);
	if (r.shortView && r.at.zone > 0) {
		r.fastestMph = fastestShortMph(r.at.zone, in.fps);
		r.fastest60 = fastestShortMph(r.at.zone, 60);
	}
	return r;
}

const char* gradeName(int g) {
	return g <= 0 ? "Good to go" : g == 1 ? "Works, a bit rougher" : "Needs a tweak";
}

std::string verdictText(const Verdict& v) {
	char b[240];
	if (v.at.zone <= 0)
		return "Only a couple of feet of street fit in the picture. Set the camera further back or zoom out, and it will fit.";
	if (v.tooFew)
		std::snprintf(b, sizeof b, "A car at %d mph crosses the %.0f ft zone in %.0f frames. Slower cars still time fine; a 60 fps "
			"camera or a little more street times this one too.", v.checkMph, v.at.zone * kFt, v.at.frames);
	else std::snprintf(b, sizeof b, "About ±%.1f mph at %d mph, over a %.0f ft zone.", v.at.total, v.checkMph, v.at.zone * kFt);
	std::string out = b;
	if (v.blurry && v.at.zone > 0) out += " Cars blur a lot at this shutter: 1/500 s or faster sharpens them.";
	return out;
}

const char* sourceName(Source s) {
	switch (s) {
	case Source::Typed: return "Your numbers";
	case Source::Picture: return "From your picture";
	case Source::Calibration: return "Measured";
	case Source::Map: return "From the map";
	default: return "Rough guess";
	}
}

const char* sourceKey(Source s) {
	switch (s) {
	case Source::Typed: return "typed";
	case Source::Picture: return "picture";
	case Source::Calibration: return "calibration";
	case Source::Map: return "map";
	default: return "guess";
	}
}

bool sourceFromKey(const std::string& key, Source& out) {
	for (Source s : { Source::Guess, Source::Typed, Source::Picture, Source::Calibration, Source::Map })
		if (key == sourceKey(s)) { out = s; return true; }
	return false;
}

const StreetPreset* findStreetPreset(const std::string& key) {
	for (const StreetPreset& p : kStreetPresets)
		if (key == p.key) return &p;
	return nullptr;
}

// The clear part of the analysis box: its width less the obstruction band (which the tracker coasts through).
static double clearPx(double boxWidthPx, double obstructionPx) {
	return std::max(0.0, boxWidthPx - std::max(0.0, obstructionPx));
}

Measured fromCalibration(double calFrames, double calSpeedMph, double calFps, double zonePx, double boxWidthPx,
	double obstructionPx) {
	Measured m;
	if (calFrames <= 0 || calSpeedMph <= 0 || calFps <= 0 || zonePx < 10) return m;
	m.zoneFt = calSpeedMph * 5280.0 / 3600.0 * calFrames / calFps;
	m.pxPerFt = zonePx / m.zoneFt;
	m.coverFt = kFrameWidthPx / m.pxPerFt;
	m.clearFt = std::min(m.coverFt, clearPx(boxWidthPx, obstructionPx) / m.pxPerFt);
	// A calibration that puts the whole picture under 3 ft or over 1000 ft across is a typo, not a street.
	m.valid = m.coverFt >= 3 && m.coverFt <= 1000 && m.clearFt > 0;
	return m;
}

Measured fromCarLengths(double carLengths, double boxWidthPx, double obstructionPx) {
	Measured m;
	if (!(carLengths > 0)) return m;
	m.coverFt = carLengths * kTypicalCarFt;
	m.pxPerFt = kFrameWidthPx / m.coverFt;
	m.clearFt = std::min(m.coverFt, clearPx(boxWidthPx > 0 ? boxWidthPx : kFrameWidthPx, obstructionPx) / m.pxPerFt);
	m.valid = m.clearFt > 0;
	return m;
}

void applyMeasured(Inputs& in, const Measured& m) {
	if (!m.valid) return;
	in.distM = std::max(distForCover(m.coverFt / kFt, in.hfovDeg), 1.0);
	in.clearM = std::max(m.clearFt / kFt, 1.0);
}

// ---- the map ------------------------------------------------------------------------------------------------

namespace {
constexpr double kEarthM = 6371008.8;   // mean Earth radius
double rad(double d) { return d * 3.14159265358979323846 / 180; }
double deg(double r) { return r * 180 / 3.14159265358979323846; }
double wrap360(double d) { d = std::fmod(d, 360); return d < 0 ? d + 360 : d; }
// Smallest angle between two headings, 0-180.
double angleBetween(double a, double b) { double d = std::fabs(wrap360(a) - wrap360(b)); return d > 180 ? 360 - d : d; }
}

double groundDistanceM(double lat1, double lon1, double lat2, double lon2) {
	double dLat = rad(lat2 - lat1), dLon = rad(lon2 - lon1);
	double a = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dLon / 2) * std::sin(dLon / 2);
	return 2 * kEarthM * std::asin(std::min(1.0, std::sqrt(a)));
}

double bearingDeg(double lat1, double lon1, double lat2, double lon2) {
	double y = std::sin(rad(lon2 - lon1)) * std::cos(rad(lat2));
	double x = std::cos(rad(lat1)) * std::sin(rad(lat2)) - std::sin(rad(lat1)) * std::cos(rad(lat2)) * std::cos(rad(lon2 - lon1));
	return wrap360(deg(std::atan2(y, x)));
}

const char* compass8(double d) {
	static const char* names[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	return names[int(std::floor(wrap360(d) / 45 + .5)) % 8];
}

MapResult fromMap(const MapPick& m) {
	MapResult r;
	bool placed = (m.camLat != 0 || m.camLon != 0) && (m.laneLat != 0 || m.laneLon != 0);
	if (!placed) return r;
	r.distM = groundDistanceM(m.camLat, m.camLon, m.laneLat, m.laneLon);
	// Under 2 ft the two pins are the same tap; over 500 ft it's the wrong street (the planner caps distance there too).
	if (r.distM < 2 / kFt || r.distM > 500 / kFt) return r;
	r.valid = true;
	r.facingDeg = bearingDeg(m.camLat, m.camLon, m.laneLat, m.laneLon);
	double right = r.facingDeg + 90;
	// The street's own direction when the map has it (a camera seldom looks exactly square-on), else square-on.
	r.l2rDeg = !m.haveWay ? right : angleBetween(m.wayBearing, right) <= 90 ? m.wayBearing : m.wayBearing + 180;
	r.l2rDeg = wrap360(r.l2rDeg);
	r.l2r = compass8(r.l2rDeg);
	r.r2l = compass8(r.l2rDeg + 180);
	// Two headings 45 degrees apart can round to the same name on a diagonal street; keep them opposite.
	if (r.l2r == r.r2l) r.r2l = compass8(r.l2rDeg + 225);
	if (m.haveWay && m.oneway != 0) {
		double legal = m.oneway > 0 ? m.wayBearing : m.wayBearing + 180;
		r.legal = angleBetween(legal, r.l2rDeg) < 90 ? "L2R" : "R2L";
	}
	if (m.maxspeedMph >= 5 && m.maxspeedMph <= 80) r.speedLimit = m.maxspeedMph;
	return r;
}

}  // namespace plan
