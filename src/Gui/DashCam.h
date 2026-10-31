// Calibrating from a Tesla dashcam clip: the car's own speedometer, frame by frame, instead of cruise control.
//
// Teslas on firmware 2025.44.25 or later (HW3 and up) write each dashcam frame's telemetry into the video as an H.264
// SEI "user data unregistered" NAL unit holding a protobuf SeiMetadata message (Tesla's published dashcam.proto,
// https://github.com/teslamotors/dashcam).  This reads those messages straight from the MP4 with no protobuf or
// video library: walk the top-level atoms for mdat (length-prefixed NAL units) and moov/mvhd (the clip's duration),
// keep the SEI NALs, strip emulation-prevention bytes, and decode the few fields calibration needs.
//
// A clip's samples are timed from its file name (TeslaCam names each one-minute clip by its local start time,
// "2026-10-31_14-03-10-front.mp4") plus its frame's share of the clip's duration.  For a pass VST measured at a
// local wall-clock time, speedDuring() averages the car's speed over the seconds around it, so the driver can drive
// past at any steady speed and VST Studio fills in what the car actually did.
//
// Matching by time needs the computer's clock and the car's to agree within a couple of seconds.  Once VST Studio
// knows where the camera's zone is on the map (the "spot", remembered from one clock-matched pass or pasted from a
// map), speedAtSpot() matches by GPS instead: it finds when the car was closest to the spot, which no clock can
// shift, and reads the speed there.  The clock only picks which drive-by it was when a clip holds several.
#pragma once
#include <string>
#include <vector>

namespace dashcam {

struct Sample {
	double t = 0;            // seconds since the Unix epoch, local wall clock (as VST's own times are)
	double mps = 0;          // vehicle_speed_mps
	double lat = 0, lon = 0; // latitude_deg, longitude_deg (0 when the car had no fix)
	bool drive = false;      // gear_state == DRIVE
};

struct Clip {
	std::string path;
	double start = 0;        // from the file name; 0 when the name has no TeslaCam timestamp
	double duration = 0;     // seconds, from mvhd
	std::vector<Sample> samples;
};

// Reads one clip.  On failure (not an MP4, no SEI telemetry, no timestamp in the name) returns false and says why
// in `err`, in words for the user.
bool readClip(const std::string& path, Clip& clip, std::string& err);

// Parses "YYYY-MM-DD_HH-MM-SS" at the start of a TeslaCam file name as local time; 0 if it isn't there.
double startFromName(const std::string& fileName);
// "YYYY-MM-DDTHH:MM:SS" (VST's vehicle times) as local time; 0 if malformed.
double parseIso(const std::string& iso);

// The car's speed over a pass: VST logs a vehicle as its front bumper crosses the second speed line, at `passEnd`
// (whole seconds), after `crossing` seconds in the zone.  The window covers the crossing, widened by `slack` seconds
// each side for the one-second log resolution and the two clocks' disagreement.
struct PassSpeed {
	bool found = false;      // samples in drive cover the window
	double mph = 0;          // mean speed over the window
	double minMph = 0, maxMph = 0;
	int samples = 0;
	bool byGps = false;      // matched by the car passing the spot, not by the clocks
	double clockOffset = 0;  // byGps: car clock minus VST's clock, seconds (how far a clock match would have been off)
	bool steady() const { return found && maxMph - minMph <= kSteadyMph; }
	static constexpr double kSteadyMph = 1.5;
};
PassSpeed speedDuring(const std::vector<Clip>& clips, double passEnd, double crossing, double slack = 2);

constexpr double kNearSpotM = 30;   // a drive-by: within this of the spot (GPS error plus a pin dropped on the house)

// Metres between two positions (equirectangular; exact enough over a street).
double distanceM(double lat1, double lon1, double lat2, double lon2);
// The car's position at time t, interpolated between the samples either side; false without a GPS fix there.
bool positionAt(const std::vector<Clip>& clips, double t, double& lat, double& lon);
// The times the car, in drive, came closest to (lat, lon) and was within maxM of it: one per drive-by.
std::vector<double> closestApproaches(const std::vector<Clip>& clips, double lat, double lon, double maxM = kNearSpotM);

// The car's speed as it passed the spot, for a pass VST logged at passEnd after `crossing` seconds in the zone: the
// drive-by nearest the clock's guess, its window centred on the moment of closest approach (the zone's middle, if
// the spot is), `crossing` wide plus `slack` each side for GPS error (a few metres: a fraction of a second).
// Falls back to speedDuring() (byGps false) when no drive-by is in the clips.
PassSpeed speedAtSpot(const std::vector<Clip>& clips, double lat, double lon, double passEnd, double crossing, double slack = 1);

}  // namespace dashcam
