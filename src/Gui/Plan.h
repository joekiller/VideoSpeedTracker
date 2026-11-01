// The site planner's model (step 2, Pick your spot), with no GUI in it, so VST Studio's planner page, the companion
// API (GET/PUT /api/plan) and tests/plan_test.cpp all work out the same numbers.
//
// What the planner needs about the street is two lengths: how far the camera is from the moving lane, and how much
// of that lane it can see clear.  Nobody knows those to the foot on day one, so the planner starts from a rough
// guess (a street preset, "narrow street from a front window") and zeroes in as setup goes on:
//
//   Source::Guess        a street preset or the first-run default; the planner says it is rough
//   Source::Typed        numbers the user typed (paced out, a map's measure tool, a tape)
//   Source::Picture      sized from the connected camera's picture: how many car lengths fit across it
//   Source::Map          measured on the map (Pick your spot's "Find it on the map"): the camera's pin to the lane's
//   Source::Calibration  measured by a calibration in step 5 (any method: a drive past, Ride-along, a Tesla clip, a
//                        known car, cones or a pole).  A calibration knows the speed zone's length in feet, and the
//                        speed lines say how many pixels that is, so it gives the scale of the whole picture.
// (Source::Map sits between Picture and Calibration in trust: a map knows the distance to a few feet but nothing about
// what blocks the view, so the clear stretch stays whatever it was.)
//
// Lengths are metres here (the model's units; the pages show feet).  "Cover" is the width of street the whole
// picture spans at the lane's distance; "clear" is the part of it with nothing in the way.
#pragma once
#include <string>

namespace plan {

constexpr double kMph = 0.44704;      // m/s per mph
constexpr double kFt = 3.28084;       // ft per m
constexpr double kLockTime = 0.2;     // s of full visibility the tracker needs before whole-frame timing starts
// Short-view timing (subFrameTiming = 1; see Guide.cpp's header for where these come from).
constexpr double kEdgeM = 0.3;        // m kept clear inside each end of the view: speed lines about 5-10% in
constexpr double kJitterPx = 12;      // front-bumper jitter in 1280-wide tracker pixels (fits on the 20 ft clip)
constexpr double kMinSamples = 4;     // bumper positions the fit needs to be trusted; the engine refuses fewer than 3
constexpr double kShortFrames = 15;   // below this many whole frames in the zone, plan with short-view timing
constexpr double kMaxTrackMph = 70;   // fastest car the tracker's margins are set for (maxDistOnEntry)
constexpr double kFrameWidthPx = 1280;   // VST's working frame width: speed lines and the analysis box are in it
constexpr double kTypicalCarFt = 15.5;   // the "car length" a picture estimate counts in (a mid-size car or small SUV)

// Everything the model reads.  The camera half can stay a guess (a phone) until the user has one.
struct Inputs {
	double distM = 6, clearM = 6.1;           // camera to the middle of the moving lane; clear stretch of it
	double hfovDeg = 66, widthPx = 1920, fps = 30, shutter = 120;   // shutter is 1/x s
	double carLenM = 4.8;
	int speedLimit = 25;
};

struct Eval {
	double cover = 0, visible = 0, zone = 0;   // m: picture width at the lane, clear part of it, timing zone
	double frames = 0, samples = 0;           // whole frames in the zone; bumper positions (short view)
	double quant = 0, calib = 0, total = 0;   // mph: timing error, calibration pass's own error, combined
	double pxPerM = 0, blurCm = 0, blurPx = 0;
	bool shortView = false;
};

// Width of street the picture spans at distance d: 2 d tan(hfov / 2).  And back again.
double coverAt(double distM, double hfovDeg);
double distForCover(double coverM, double hfovDeg);

// The model at one speed, with short-view timing or whole-frame timing.
Eval evaluate(const Inputs& in, double mph, bool shortView);

// The planner's answer at the speed it checks (limit + 10): which timing it plans with, the error, a grade
// (0 good, 1 rougher, 2 needs a tweak) and the fastest car a short view times at this frame rate and at 60 fps.
struct Verdict {
	int checkMph = 35;
	bool shortView = false;   // planned with short-view timing (on already, or needed because the view is short)
	bool tooFew = false;      // short view with too few bumper positions at checkMph
	bool blurry = false;      // motion blur over 25 px at checkMph: graded no better than "a bit rougher"
	int grade = 2;
	Eval at;                  // evaluate() at checkMph with the timing above
	double fastestMph = 0;    // short view only: fastest car timed at in.fps (0 = not a short view)
	double fastest60 = 0;     // the same at 60 fps
};
Verdict verdict(const Inputs& in, bool shortTimingOn);

// The verdict in words, the same on the planner page and the phone: its pill ("Good to go", "Works, a bit rougher",
// "Needs a tweak") and its sentence ("About ±1.4 mph at 35 mph, over a 52 ft zone.").
const char* gradeName(int grade);
std::string verdictText(const Verdict& v);

// The fastest car a short view of `zoneM` times at `fps`: it needs 2 (kMinSamples - 1) frames in the zone.
double fastestShortMph(double zoneM, double fps);
// "up to about 45 mph" / "over 70 mph", as the planner and the frame-rate advice say it.
std::string fastestText(double mph);

// 0 good, 1 marginal, 2 bad; lower values are better.
inline int grade(double x, double good, double warn) { return x <= good ? 0 : x <= warn ? 1 : 2; }

// ---- where the street numbers came from ---------------------------------------------------------------------

enum class Source { Guess = 0, Typed = 1, Picture = 2, Calibration = 3, Map = 4 };
constexpr int kSourceCount = 5;
inline Source sourceOf(int saved) { return Source(saved < 0 || saved >= kSourceCount ? 0 : saved); }
const char* sourceName(Source s);   // "Rough guess", "Your numbers", "From your picture", "Measured", "From the map"
const char* sourceKey(Source s);    // "guess", "typed", "picture", "calibration", "map" (the API's spelling)
bool sourceFromKey(const std::string& key, Source& out);

// A view a first-time user can pick without measuring anything.  Feet, because that is how they're shown.
struct StreetPreset { const char* key; const char* name; const char* hint; double distFt, clearFt; };
inline constexpr StreetPreset kStreetPresets[] = {
	{ "narrow", "Narrow street from a front window",
	  "An old-city or one-way street with parking, seen from a front window across a small yard: about 20 ft to the "
	  "moving lane, and trees or porches leave about 20 ft of it clear.", 20, 20 },
	{ "neighborhood", "Two-lane street from the house",
	  "A two-way neighborhood street seen from a front window or porch: about 35 ft to the nearer lane, 50 ft of it clear.", 35, 50 },
	{ "wide", "Wide street or a long, open view",
	  "A wider street, or a view along a fence or a park with nothing in the way: about 50 ft to the lane, 90 ft clear.", 50, 90 },
};
constexpr int kStreetPresetCount = int(sizeof kStreetPresets / sizeof kStreetPresets[0]);
const StreetPreset* findStreetPreset(const std::string& key);

// ---- measuring the street from what setup already has ------------------------------------------------------

// What a calibration says about the picture's scale.  A calibration of `calFrames` frames at `calSpeedMph` and
// `calFps` means a car at that speed crosses the speed zone in calFrames / calFps seconds, so the zone is
// calSpeed (ft/s) x that long, and it is `zonePx` pixels wide on the picture.  The whole 1280-pixel frame then spans
// kFrameWidthPx / pxPerFt feet at the lane's depth (the cover), and the analysis box less any obstruction is the
// clear stretch.  `valid` is false for nonsense input (no zone, no calibration).
struct Measured {
	bool valid = false;
	double pxPerFt = 0;
	double coverFt = 0, clearFt = 0, zoneFt = 0;
};
Measured fromCalibration(double calFrames, double calSpeedMph, double calFps, double zonePx, double boxWidthPx,
	double obstructionPx);

// The same from the picture alone: `carLengths` cars of kTypicalCarFt fit across the whole frame.  Rougher (cars
// differ by a few feet, and the user is judging by eye), which is why it only stands until a calibration.
Measured fromCarLengths(double carLengths, double boxWidthPx, double obstructionPx);

// Applies a measurement to the inputs: the cover sets the camera distance through the field of view (so the
// planner's picture width matches what was measured) and the clear stretch is taken as it is.
void applyMeasured(Inputs& in, const Measured& m);

// ---- the map --------------------------------------------------------------------------------------------------
//
// Pick your spot's map (companion/js/views/map.js, opened in a browser from Studio or on the phone) places two pins
// on OpenStreetMap: the camera (the window) and the middle of the moving lane in front of it, and reads the street
// under the lane pin from OpenStreetMap: its name, its direction there, whether it is one-way and its posted limit.
// From that the planner gets the distance to the lane, and Mark your street gets which compass heading a car going
// left to right in the picture has, and which way is legal.  The camera looks from its pin toward the lane pin, so
// "right" in the picture is that bearing plus 90 degrees; a car going left to right moves along whichever of the
// street's two directions is nearer to it.

struct MapPick {
	double camLat = 0, camLon = 0, laneLat = 0, laneLon = 0;
	bool haveWay = false;       // OpenStreetMap had a street under the lane pin
	double wayBearing = 0;      // degrees from north, clockwise, along the street's drawn direction there
	int oneway = 0;             // 0 two-way, 1 one-way along wayBearing, -1 one-way against it
	int maxspeedMph = 0;        // posted limit (0 = not on the map)
	std::string street;         // the street's name ("" = unnamed)
};

struct MapResult {
	bool valid = false;         // both pins placed, 2-500 ft apart
	double distM = 0;           // camera to the lane
	double facingDeg = 0;       // the camera's heading (camera pin toward the lane pin)
	double l2rDeg = 0;          // heading of a car crossing the picture left to right
	std::string l2r, r2l;       // the same as 8-point compass names for VST.cfg (L2RDirection, R2LDirection)
	std::string legal = "Both"; // LegalDirection for a one-way street: "L2R" or "R2L"
	int speedLimit = 0;         // the posted limit, when the map has one
};

double groundDistanceM(double lat1, double lon1, double lat2, double lon2);   // great-circle (haversine)
double bearingDeg(double lat1, double lon1, double lat2, double lon2);        // initial bearing, 0-360
const char* compass8(double deg);                                             // "N", "NE", ... "NW"
MapResult fromMap(const MapPick& m);

}  // namespace plan
