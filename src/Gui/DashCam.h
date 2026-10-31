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
	bool steady() const { return found && maxMph - minMph <= kSteadyMph; }
	static constexpr double kSteadyMph = 1.5;
};
PassSpeed speedDuring(const std::vector<Clip>& clips, double passEnd, double crossing, double slack = 2);

}  // namespace dashcam
