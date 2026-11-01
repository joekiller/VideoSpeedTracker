// Tesla dashcam telemetry (src/Gui/DashCam.cpp) unit test.
//
//   dashcam_test <scratch dir>
//
// Builds TeslaCam-style MP4s byte by byte (ftyp, mdat of length-prefixed NAL units with one SEI SeiMetadata message
// per frame between the pictures, moov/mvhd with the clip's duration) and checks that the reader finds every
// message, undoes H.264's emulation-prevention bytes, skips fields it doesn't use, times the samples from the file
// name and duration, and that a pass's speed is read from the right seconds.  GPS matching: a car driving along a
// street, its pass found by when it came closest to the camera spot even when the computer's clock is 8 s off (a
// clock match would land in the acceleration), the right drive-by of two, and the fall-back when the spot is far.
// Ride-along: a phone's 1 Hz GPS recording of the same drive read alone and together with the Tesla's clip, agreeing,
// and disagreeing when the phone reads high.  Also the user-facing errors: not an
// MP4, no telemetry (older firmware), no TeslaCam timestamp in the name.
#include "DashCam.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void put32(Bytes& b, uint32_t v) { for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(v >> s)); }
static void putVarint(Bytes& b, uint64_t v) { do { b.push_back(uint8_t((v & 0x7F) | (v > 0x7F ? 0x80 : 0))); v >>= 7; } while (v); }
static void putRaw(Bytes& b, const void* p, size_t n) { auto c = static_cast<const unsigned char*>(p); b.insert(b.end(), c, c + n); }

static Bytes box(const char* type, const Bytes& body) {
	Bytes b;
	put32(b, uint32_t(body.size() + 8));
	putRaw(b, type, 4);
	b.insert(b.end(), body.begin(), body.end());
	return b;
}

// A SeiMetadata message as Tesla's dashcam.proto encodes it, with fields the reader must skip in between.
static Bytes message(uint64_t seq, float mps, bool drive, double lat, double lon) {
	Bytes m;
	putVarint(m, 1 << 3); putVarint(m, 1);                        // version
	if (drive) { putVarint(m, 2 << 3); putVarint(m, 1); }        // gear_state DRIVE (PARK = 0 is left out)
	putVarint(m, 3 << 3); putVarint(m, seq);                      // frame_seq_no
	if (mps != 0) { putVarint(m, 4 << 3 | 5); putRaw(m, &mps, 4); }
	float pedal = .2f;
	putVarint(m, 5 << 3 | 5); putRaw(m, &pedal, 4);               // accelerator_pedal_position
	putVarint(m, 11 << 3 | 1); putRaw(m, &lat, 8);
	putVarint(m, 12 << 3 | 1); putRaw(m, &lon, 8);
	double accel = 0;                                             // linear_acceleration_mps2_x: eight zero bytes
	putVarint(m, 14 << 3 | 1); putRaw(m, &accel, 8);
	return m;
}

// Wraps a message in an SEI NAL the way Tesla does (06 05 size 42.. 69 payload 80), escaping any 00 00 0x run.
static Bytes seiNal(const Bytes& msg) {
	Bytes n = { 0x06, 0x05, uint8_t(msg.size() + 4), 0x42, 0x42, 0x69 };
	int zeros = 0;
	for (unsigned char c : msg) {
		if (zeros >= 2 && c <= 3) { n.push_back(3); zeros = 0; }
		n.push_back(c);
		zeros = c == 0 ? zeros + 1 : 0;
	}
	n.push_back(0x80);
	return n;
}

// A clip of `frames` frames over `seconds`, the speed of frame i given by `speed(i)` (m/s).  Each frame is an SEI
// unit followed by a stand-in picture NAL, as in a real clip.
using Pos = std::function<std::pair<double, double>(int)>;
static void writeClip(const fs::path& p, int frames, double seconds, const std::function<float(int)>& speed, bool withSei = true,
	const Pos& pos = nullptr) {
	Bytes mdat;
	for (int i = 0; i < frames; i++) {
		if (withSei) {
			auto at = pos ? pos(i) : std::make_pair(38.0302, -78.4836);
			Bytes nal = seiNal(message(1000 + i, speed(i), speed(i) > 0, at.first, at.second));
			put32(mdat, uint32_t(nal.size()));
			mdat.insert(mdat.end(), nal.begin(), nal.end());
		}
		Bytes pic(300, uint8_t(i));
		pic[0] = 0x65;   // IDR slice
		put32(mdat, uint32_t(pic.size()));
		mdat.insert(mdat.end(), pic.begin(), pic.end());
	}
	Bytes mvhd(100, 0);
	uint32_t scale = 1000, dur = uint32_t(seconds * 1000);
	for (int s = 0; s < 4; s++) { mvhd[12 + s] = uint8_t(scale >> (24 - 8 * s)); mvhd[16 + s] = uint8_t(dur >> (24 - 8 * s)); }
	Bytes ftyp = { 'i', 's', 'o', 'm', 0, 0, 2, 0, 'i', 's', 'o', 'm' };
	Bytes file = box("ftyp", ftyp), d = box("mdat", mdat), m = box("moov", box("mvhd", mvhd));
	file.insert(file.end(), d.begin(), d.end());
	file.insert(file.end(), m.begin(), m.end());
	std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(file.data()), std::streamsize(file.size()));
}

int main(int argc, char** argv) {
	if (argc < 2) { std::printf("usage: dashcam_test <scratch dir>\n"); return 2; }
	fs::path out = argv[1];
	fs::create_directories(out);
	const double mps25 = 25 / 2.2369362920544;

	// Names and times.
	double t0 = dashcam::startFromName("2026-10-31_14-03-10-front.mp4");
	CHECK(t0 > 0);
	CHECK(dashcam::parseIso("2026-10-31T14:03:40") - t0 == 30);
	CHECK(dashcam::startFromName("front.mp4") == 0);
	CHECK(dashcam::parseIso("14:03:40") == 0);

	// A one-minute clip at 36 fps: parked for 5 s, then accelerating to 25 mph by 15 s, steady at 25 until 45 s,
	// then braking.
	const int fps = 36, n = 60 * fps;
	auto speed = [&](int i) {
		double t = double(i) / fps;
		if (t < 5) return 0.f;
		if (t < 15) return float(mps25 * (t - 5) / 10);
		if (t < 45) return float(mps25 + (i % 2 ? .05 : -.05));   // speedometer noise, ±0.1 mph
		return float(std::max(0.0, mps25 * (55 - t) / 10));
	};
	fs::path clipPath = out / "2026-10-31_14-03-10-front.mp4";
	writeClip(clipPath, n, 60, speed);
	dashcam::Clip clip;
	std::string err;
	CHECK(dashcam::readClip(clipPath.string(), clip, err));
	std::printf("  read %zu samples over %.1f s (%s)\n", clip.samples.size(), clip.duration, err.c_str());
	CHECK(int(clip.samples.size()) == n);
	CHECK(std::fabs(clip.duration - 60) < 1e-9);
	CHECK(clip.start == t0);
	if (int(clip.samples.size()) == n) {
		CHECK(!clip.samples[0].drive && clip.samples[0].mps == 0);       // parked: proto3 leaves out gear and speed
		CHECK(clip.samples[30 * fps].drive);
		CHECK(std::fabs(clip.samples[30 * fps].mps - mps25) < .1);
		CHECK(std::fabs(clip.samples[30 * fps].lat - 38.0302) < 1e-12);  // a double full of zero bytes, escaped
		CHECK(std::fabs(clip.samples[30 * fps].lon + 78.4836) < 1e-12);
		CHECK(std::fabs(clip.samples[n - 1].t - (t0 + 60.0 * (n - 1) / n)) < 1e-6);
	}

	// A pass logged 30 s into the clip after 0.8 s in the zone: 25 mph and steady.
	std::vector<dashcam::Clip> clips = { clip };
	dashcam::PassSpeed p = dashcam::speedDuring(clips, t0 + 30, .8);
	std::printf("  pass at 30 s: %.2f mph (%.2f..%.2f, %d samples)\n", p.mph, p.minMph, p.maxMph, p.samples);
	CHECK(p.found && p.steady());
	CHECK(std::fabs(p.mph - 25) < .1);
	// While accelerating: found, but not steady, so Studio warns rather than trusting it.
	p = dashcam::speedDuring(clips, t0 + 10, .8);
	CHECK(p.found && !p.steady());
	// Outside the clip, or only touching its end: not found.
	CHECK(!dashcam::speedDuring(clips, t0 + 120, .8).found);
	CHECK(!dashcam::speedDuring(clips, t0 + 60, .8).found);
	CHECK(!dashcam::speedDuring({}, t0 + 30, .8).found);

	// A pass that straddles two clips is covered by the pair.
	fs::path next = out / "2026-10-31_14-04-10-front.mp4";
	writeClip(next, n, 60, [&](int) { return float(mps25); });
	dashcam::Clip clip2;
	CHECK(dashcam::readClip(next.string(), clip2, err));
	clips.push_back(clip2);
	p = dashcam::speedDuring(clips, t0 + 60, .8);
	CHECK(p.found && p.steady());

	// Errors a user can act on.
	fs::path old = out / "2025-06-01_09-00-00-front.mp4";
	writeClip(old, 100, 3, speed, false);
	CHECK(!dashcam::readClip(old.string(), clip, err) && err.find("2025.44.25") != std::string::npos);
	fs::path renamed = out / "my drive.mp4";
	fs::copy_file(clipPath, renamed, fs::copy_options::overwrite_existing);
	CHECK(!dashcam::readClip(renamed.string(), clip, err) && err.find("date and time") != std::string::npos);
	fs::path text = out / "notes.mp4";
	std::ofstream(text) << "not a video";
	CHECK(!dashcam::readClip(text.string(), clip, err) && err.find("isn't an MP4") != std::string::npos);
	CHECK(!dashcam::readClip((out / "missing.mp4").string(), clip, err) && err.find("Can't open") != std::string::npos);

	// GPS.  A street running east along 38.03 N; the car's position is its speed integrated, and the GPS updates
	// every 4th frame (about 9 Hz), repeating in between as a real receiver does.
	CHECK(std::fabs(dashcam::distanceM(38.03, -78.48, 38.031, -78.48) - 111.2) < .5);
	const double lat0 = 38.03, lon0 = -78.49, mPerDegLon = 111195 * std::cos(lat0 * 3.14159265358979 / 180);
	std::vector<double> east(n + 1, 0);
	for (int i = 1; i <= n; i++) east[i] = east[i - 1] + speed(i - 1) / double(fps);
	auto drivePos = [&](int i) { int g = i - i % 4; return std::make_pair(lat0, lon0 + east[g] / mPerDegLon); };
	fs::path gpsPath = out / "2026-10-31_15-00-00-front.mp4";
	writeClip(gpsPath, n, 60, speed, true, drivePos);
	dashcam::Clip gclip;
	CHECK(dashcam::readClip(gpsPath.string(), gclip, err));
	double g0 = gclip.start;
	std::vector<dashcam::Clip> gclips = { gclip };
	// The spot: where the car was 20 s in (steady at 25 mph).
	double spotLat = lat0, spotLon = lon0 + east[20 * fps] / mPerDegLon, lat, lon;
	CHECK(dashcam::positionAt(gclips, g0 + 20, lat, lon) && dashcam::distanceM(lat, lon, spotLat, spotLon) < 2);
	std::vector<double> passes = dashcam::closestApproaches(gclips, spotLat, spotLon);
	CHECK(passes.size() == 1 && std::fabs(passes[0] - (g0 + 20)) < .15);
	// The pass crossed a 0.6 s zone centred on 20 s, so VST logged it at 20.3 s, truncated to 20; but the computer's
	// clock is 8 s behind the car's, so it logged 12.  By clock that's the acceleration; by GPS it's 25 mph steady.
	double logged = std::floor(20.3) - 8;
	CHECK(!dashcam::speedDuring(gclips, g0 + logged, .6).steady());
	dashcam::PassSpeed gp = dashcam::speedAtSpot(gclips, spotLat, spotLon, g0 + logged, .6);
	std::printf("  GPS match: %.2f mph, clock offset %+.2f s\n", gp.mph, gp.clockOffset);
	CHECK(gp.byGps && gp.found && gp.steady());
	CHECK(std::fabs(gp.mph - 25) < .1);
	CHECK(std::fabs(gp.clockOffset - 8) < .6);
	// A spot a kilometre away: no drive-by, so it falls back to the clocks.
	CHECK(!dashcam::speedAtSpot(gclips, lat0 + .01, spotLon, g0 + logged, .6).byGps);
	// Back the other way in the next minute at 30 mph, past the spot at 90 s: the clock's guess picks this drive-by.
	const double mps30 = 30 / 2.2369362920544;
	double eastAtSpot = east[20 * fps];
	fs::path backPath = out / "2026-10-31_15-01-00-front.mp4";
	writeClip(backPath, n, 60, [&](int) { return float(mps30); }, true,
		[&](int i) { int g = i - i % 4; return std::make_pair(lat0, lon0 + (eastAtSpot + mps30 * (30 - double(g) / fps)) / mPerDegLon); });
	dashcam::Clip back;
	CHECK(dashcam::readClip(backPath.string(), back, err));
	gclips.push_back(back);
	passes = dashcam::closestApproaches(gclips, spotLat, spotLon);
	CHECK(passes.size() == 2);
	gp = dashcam::speedAtSpot(gclips, spotLat, spotLon, g0 + 90 - 3, .6);
	CHECK(gp.byGps && gp.steady() && std::fabs(gp.mph - 30) < .1);
	gp = dashcam::speedAtSpot(gclips, spotLat, spotLon, g0 + 20 - 3, .6);
	CHECK(gp.byGps && std::fabs(gp.mph - 25) < .1);

	// A phone's VST Ride-along recording of the same drive: one fix a second (the first 20 s of the minute, the car
	// past the spot at 20 s), GPS speed with ±0.1 m/s of noise, one fix with no speed (filled from its neighbours)
	// and one too inaccurate to keep.
	auto writeRide = [&](const fs::path& path, double bias) {
		std::ofstream r(path);
		r << "# VST ride-along recording v1\ntime_ms,lat,lon,speed_mps,accuracy_m,heading_deg\n";
		for (int sec = 0; sec < 60; sec++) {
			int i = sec * fps;
			char line[160];
			double v = speed(i) + (sec % 2 ? .1 : -.1) + bias;
			if (sec == 22) snprintf(line, sizeof line, "%.0f,%.7f,%.7f,,6.0,90\n", (g0 + sec) * 1000, lat0, lon0 + east[i] / mPerDegLon);
			else snprintf(line, sizeof line, "%.0f,%.7f,%.7f,%.2f,%.1f,90\n", (g0 + sec) * 1000, lat0, lon0 + east[i] / mPerDegLon, std::max(0.0, v), sec == 40 ? 60.0 : 5.0);
			r << line;
		}
	};
	fs::path ridePath = out / "vst-ride-2026-10-31_15-00-00.csv";
	writeRide(ridePath, 0);
	dashcam::Clip ride;
	CHECK(dashcam::readAny(ridePath.string(), ride, err));
	CHECK(ride.kind == dashcam::Clip::Phone);
	CHECK(ride.samples.size() == 59);                                  // the 60 m fix is dropped
	if (ride.samples.size() == 59) CHECK(std::fabs(ride.samples[22].mps - mps25) < .2);   // filled from positions
	CHECK(std::fabs(ride.start - g0) < 1e-6);
	dashcam::Clip teslaOnly;
	CHECK(dashcam::readAny(gpsPath.string(), teslaOnly, err) && teslaOnly.kind == dashcam::Clip::Tesla);

	// Phone alone, matched by GPS with the clock 8 s off as above.
	dashcam::Joint jn = dashcam::jointSpeed({ ride }, spotLat, spotLon, g0 + logged, .6);
	std::printf("  phone alone: %.2f mph (%s)\n", jn.mph, jn.phone.byGps ? "GPS" : "clock");
	CHECK(jn.found && jn.steady && !jn.both && jn.phone.byGps && std::fabs(jn.mph - 25) < .3);
	// Tesla and phone together: both steady, agree, the mean.
	jn = dashcam::jointSpeed({ gclip, ride }, spotLat, spotLon, g0 + logged, .6);
	std::printf("  joint: Tesla %.2f, phone %.2f -> %.2f mph\n", jn.tesla.mph, jn.phone.mph, jn.mph);
	CHECK(jn.both && jn.agree && std::fabs(jn.mph - (jn.tesla.mph + jn.phone.mph) / 2) < 1e-9);
	// A phone reading 3 mph high (bad sky view): they disagree and the Tesla's reading is used.
	fs::path biased = out / "vst-ride-2026-10-31_15-00-01.csv";
	writeRide(biased, 3 / 2.2369362920544);
	dashcam::Clip bad;
	CHECK(dashcam::readRide(biased.string(), bad, err));
	jn = dashcam::jointSpeed({ gclip, bad }, spotLat, spotLon, g0 + logged, .6);
	CHECK(jn.both && !jn.agree && jn.mph == jn.tesla.mph);
	// Without a spot both are matched by clock: with the clocks right that's the same pass.
	jn = dashcam::jointSpeed({ gclip, ride }, 0, 0, g0 + 20, .6);
	CHECK(jn.agree && !jn.tesla.byGps && !jn.phone.byGps);
	// Not a recording.
	fs::path notRide = out / "notes.csv";
	std::ofstream(notRide) << "a,b,c\n1,2,3\n";
	CHECK(!dashcam::readAny(notRide.string(), ride, err) && err.find("isn't a VST Ride-along") != std::string::npos);

	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
