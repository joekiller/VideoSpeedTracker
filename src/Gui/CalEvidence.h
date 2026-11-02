// Calibration evidence: every way of calibrating adds a measurement, and the calibration in force is their
// weighted combination, instead of the last method used replacing everything before it.
//
// Each method (a drive past at a known speed, a Ride-along or Tesla pass, "That was me", two cones or a pole, a car
// of known length, the ghost-car camera setup, ...) ends up saying the same physical thing: how many feet of street
// lie between the two speed lines.  That length is independent of CalibrationSpeed and CalibrationFPS, so inputs made
// at different frame rates or calibration speeds combine directly:
//
//     pass at v mph taking n frames at f fps   ->  zoneFt = v × 5280/3600 × n / f
//     marks markPx apart that are D ft apart   ->  zoneFt = zonePx / markPx × D
//     a pinhole camera model                   ->  zoneFt as perspective::zoneFeet() gives it
//
// Each input also carries `sigma`, its relative 1-sigma uncertainty (0.03 = ±3%), from the method's error model
// (sigmaPass / sigmaMarks / kSigma*).  The combination is the inverse-variance weighted mean, so a careful drive past
// counts for more than a rough camera-setup guess, and each input's share of the weight is its "contribution":
//
//     w_i = 1 / sigma_i²,   zoneFt = Σ w_i x_i / Σ w_i,   contribution_i = w_i / Σ w_i
//
// (sigma relative, and the inputs within a few percent of each other, so relative weights are as good as absolute
// ones and equal sigmas give the plain mean).  The combined relative uncertainty is 1 / sqrt(Σ w_i), widened by the Birge ratio sqrt(χ² / (n - 1)) when the inputs
// disagree by more than their own uncertainties allow, so two careful methods that disagree by 10% never claim ±1%.
// An input that sits well off the others (leave-one-out, more than 3 sigma and kOutlierPct) is flagged so the page
// can say which one, but it is never dropped silently: the user removes it, and can put it back.
//
// An input only counts on the speed lines it was measured with (its `lines` signature, App::speedLinesSig): moving
// a speed line changes the zone, so inputs from other lines are set aside, as calibratedLines did for the single
// calibration before.  Removing an input is a flag, not a deletion, so it can be undone.
//
// Storage (CalEvidence.cpp): <configDir>/calibration-inputs.json, keyed by canonical config path like
// calibration-history.csv.  The history stays the append-only audit record (an "apply" row whenever the combined
// whole-frame value changes); this file is the editable working set.
#pragma once
#include <cmath>
#include <string>
#include <vector>

namespace calib {

constexpr double kFtPerSecPerMph = 5280.0 / 3600.0;
constexpr int kL2R = 1, kR2L = 2, kBothDirs = 3;

// Default uncertainties for the methods that don't work one out from their own numbers.
constexpr double kSigmaCamera = 0.10;    // ghost-car pinhole setup: typed distance, height, angle and lens
constexpr double kSigmaEarlier = 0.06;   // a calibration made before inputs were kept, method unknown
constexpr double kOutlierPct = 4.0;      // an input this far (and 3 sigma) off the rest is called out

struct Input {
	std::string id;          // unique within the config (time + counter), for removing it
	std::string method;      // "pass", "check", "cones", "car", "camera", "earlier"; other threads may add more
	std::string label;       // novice-facing: "Drive past at 15 mph", "Toyota Camry 2018-2024, parked"
	std::string time;        // ISO 8601, calib::nowIso()
	int dirs = kBothDirs;    // kL2R, kR2L or both
	double zoneFt = 0;       // feet of street between the speed lines that this input says
	double sigma = 0.05;     // relative 1-sigma uncertainty
	std::string lines;       // speed lines it was measured on (App::speedLinesSig)
	bool removed = false;    // taken out by the user; kept so "Put it back" works
	bool appliesTo(bool l2r) const { return (dirs & (l2r ? kL2R : kR2L)) != 0; }
};

// ---- each method's measurement and error model -----------------------------------------------------------------

inline double zoneFtFromPass(double mph, double frames, double fps) {
	if (mph <= 0 || frames <= 0 || fps <= 0) return 0;
	return mph * kFtPerSecPerMph * frames / fps;
}
inline double zoneFtFromMarks(double markPx, double markFt, double zonePx) {
	if (markPx <= 0 || markFt <= 0 || zonePx <= 0) return 0;
	return zonePx / markPx * markFt;
}
// CalibrationFrames for a zone of `zoneFt`: a car at calSpeed crosses it in zoneFt / (calSpeed ft/s) s, × calFps.
inline double framesForZone(double zoneFt, double calSpeed, double calFps) {
	if (zoneFt <= 0 || calSpeed <= 0 || calFps <= 0) return 0;
	return zoneFt / (calSpeed * kFtPerSecPerMph) * calFps;
}
inline double zoneFtForFrames(double calFrames, double calSpeed, double calFps) {
	if (calFrames <= 0 || calSpeed <= 0 || calFps <= 0) return 0;
	return calSpeed * kFtPerSecPerMph * calFrames / calFps;
}

// A pass at a known speed.  Two independent errors: the speed (a speedometer or cruise control is good to about 2%,
// plus half a mph of holding it steady) and counting whole frames (the bumper crosses each line somewhere within a
// frame, so the count is off by the difference of two uniform errors: sqrt(2/12) ≈ 0.41 frame).
inline double sigmaPass(double mph, double frames) {
	if (mph <= 0 || frames <= 0) return 1;
	double speed = std::hypot(0.02 * mph, 0.5) / mph;
	double count = 0.41 / frames;
	return std::hypot(speed, count);
}
// Two marks a known distance apart (cones, a pole, a car's bumpers): the distance itself (`lengthRel`: 1% for a tape,
// 0.5% for a maker's length), clicking each mark to about 3 px (two of them, so ~4 px on the span), and how well the
// marks sit at the lane's depth (`depthRel`: the part a novice gets wrong, a few percent).
inline double sigmaMarks(double markPx, double lengthRel, double depthRel) {
	if (markPx <= 0) return 1;
	return std::sqrt(lengthRel * lengthRel + (4.0 / markPx) * (4.0 / markPx) + depthRel * depthRel);
}
// Cones or a pole on a tape-measured spacing, stood where the cars' near wheels run.
inline double sigmaCones(double markPx) { return sigmaMarks(markPx, 0.01, 0.025); }
// A car of known length.  Moving, or parked in the lane: its bumpers are at the lane's depth (2% for where on the
// bumper corners one clicks).  Parked beyond the lane, the depth correction's guess of `fartherFt` is good to about
// 1.5 ft, which moves the result by 1.5 / (laneFt + fartherFt); with no lane distance known, 8%.
inline double sigmaCar(double markPx, bool parked, double laneFt, double fartherFt) {
	double depth = 0.02;
	if (parked && fartherFt > 0) depth = laneFt > 0 ? std::hypot(0.02, 1.5 / (laneFt + fartherFt)) : 0.08;
	return sigmaMarks(markPx, 0.005, depth);
}

// ---- combining --------------------------------------------------------------------------------------------------

struct Share {
	std::string id;
	double contribution = 0;   // its share of the weight, 0..1
	double offPct = 0;         // how far it sits from the combined value, signed percent
	bool outlier = false;      // well off the others (needs 3+ inputs to say which)
};

struct Combined {
	bool valid = false;
	int n = 0;                 // inputs that counted
	double zoneFt = 0;
	double sigma = 0;          // relative 1-sigma of the combination, Birge-widened
	double spreadPct = 0;      // the inputs' range (highest - lowest) as a percent of the combination
	double birge = 1;          // sqrt(χ² / (n - 1)): about 1 when the inputs agree as well as their sigmas say
	std::vector<Share> shares; // one per counted input, in input order
	int outliers = 0;          // inputs flagged as well off the others
	int setAside = 0;          // inputs for this direction made on other speed lines
	// The inputs agree: within 3% of each other, or within what their own uncertainties allow (Birge ratio up to 1.5)
	// as long as that is no more than 6% (beyond that, "agree" would read wrong to anyone, whatever the sigmas say).
	bool agree() const { return n >= 2 && outliers == 0 && (spreadPct <= 3.0 + 1e-9 || (birge <= 1.5 && spreadPct <= 6.0 + 1e-9)); }
};


// The calibration one direction gets from `inputs`: those not removed, made for that direction, on `lines`.
Combined combine(const std::vector<Input>& inputs, bool l2r, const std::string& lines);

// The verdict in a few words for a novice, from the combination: "3 measurements agree within 2%" (the range of
// them), "1 measurement so far", "2 measurements differ by 9%".  `detail` gets the next step, or "" when there's none.
std::string summary(const Combined& c, std::string* detail = nullptr);

// Plain names for a method, for the inputs list ("Drive past", "Cones or pole", ...).
const char* methodName(const std::string& method);

// ---- storage ----------------------------------------------------------------------------------------------------

std::string inputsPath();   // <configDir>/calibration-inputs.json
// The inputs on record for `configPath` in the file at `path` (paths compared canonically), oldest first.  `known` is
// set when the config has an entry at all (even an empty list), so a first-time seed from the history happens once.
std::vector<Input> loadInputs(const std::string& path, const std::string& configPath, bool* known = nullptr);
// Replaces `configPath`'s entry in the file at `path`, keeping every other config's.  Writes a temp file and renames
// it over the old one, so a crash mid-write never loses the record.  false with `err` if it can't.
bool saveInputs(const std::string& path, const std::string& configPath, const std::vector<Input>& inputs, std::string& err);
// Adds `in` to `inputs` (giving it an id), unless an active input already says exactly the same thing (same method,
// directions, value, sigma and lines: the same pass applied twice), which would count one measurement twice; then
// that one is kept and false is returned.
bool addInput(std::vector<Input>& inputs, Input in);
// A fresh id for an input: the time plus a counter, unique within `inputs`.
std::string newInputId(const std::vector<Input>& inputs, const std::string& time);

}  // namespace calib
