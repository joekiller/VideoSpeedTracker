// Speed trend (src/Wintel64/VideoSpeedTracker/SpeedTrend.h): entry and exit speed, acceleration class, and the speed
// projected 20 ft further on.
//
//   speed_trend_test [--table]
//
// 1. Exact tracks (no noise): a car at a known speed and acceleration, sampled every frame pair, gives back that
//    acceleration, the right entry, exit and projected speeds, and the right class on each side of every threshold.
// 2. Noisy tracks, the short views VST is for: 14 and 20 ft zones at 30 and 60 fps, front bumpers seen every frame
//    pair from 12% of the zone before the entry line (tests/short_view.cmake's box) up to the first one past the exit
//    line (the tracker times the car there), each with Gaussian jitter.  The jitter, 0.1 ft rms, is about 5 px in
//    the 20 ft short view and three times what the sample's short-view crop shows (its fits stray 1-2 px rms).
//    Four hundred tracks per case, from a fixed seed with a hand-made Gaussian so every platform draws the same ones.
//    What must hold:
//      - a steady car is called accelerating or decelerating in at most 4% of passes in any view (the t rule);
//      - braking at 8 mph/s and flooring it at 10 mph/s are seen as such in a 20 ft zone at 30 fps;
//      - at 60 fps a 4 mph/s pull-away is seen as speeding up in most passes;
//      - a 14 ft zone at 30 fps may say "can't tell" (Unknown) or "steady", but never the wrong direction.
//    --table prints the rates for every case (the numbers quoted in docs/speed-trend.md).
#include "SpeedTrend.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

using namespace vst;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { checks++; double a_ = (a), b_ = (b); if (std::fabs(a_ - b_) > (tol)) { failures++; \
	std::printf("FAIL %s:%d  %s = %.4f, want %.4f +- %.4f\n", __FILE__, __LINE__, #a, a_, b_, double(tol)); } } while (0)

// A front bumper track: first seen pre * zone before the entry line, then every frame pair (2 / fps s) until the first
// one past the exit line.  mph at the entry line, accel in mph/s (constant).  phase in [0, 1) shifts the samples.
struct Track { std::vector<double> t, x; };
static Track makeTrack(double zoneFt, double fps, double mph, double accel, double phase, double pre) {
	Track tr;
	const double v0 = mph * kFtPerSecPerMph, a = accel * kFtPerSecPerMph, dt = 2 / fps;
	const double start = -pre * zoneFt;
	// Time at which the car is at `start`, with t = 0 at the entry line: start = v0 t + a t^2 / 2.
	double t0 = std::fabs(a) < 1e-9 ? start / v0 : (-v0 + std::sqrt(std::max(v0 * v0 + 2 * a * start, 0.0))) / a;
	for (int i = 0; i < 400; i++) {
		double t = t0 + (i + phase) * dt;
		double x = v0 * t + a * t * t / 2;
		if (i > 0 && x <= tr.x.back()) break;   // stopped
		tr.t.push_back(t);
		tr.x.push_back(x);
		if (x > zoneFt) break;
	}
	return tr;
}

// Gaussian from mt19937's raw output (the same on every standard library, unlike std::normal_distribution).
struct Noise {
	std::mt19937 rng{ 20261101u };
	double uniform() { return (double(rng()) + 0.5) / 4294967296.0; }
	double gauss() { return std::sqrt(-2 * std::log(uniform())) * std::cos(2 * 3.14159265358979323846 * uniform()); }
};

static const double kNoiseFt = 0.1, kFloorFt = 0.05, kMaxRmsFt = 0.6;

static SpeedTrend run(const Track& tr, double zoneFt) { return speedTrend(tr.t, tr.x, zoneFt, kFloorFt, kMaxRmsFt); }
static Track makeTrack(double zoneFt, double fps, double mph, double accel, double phase) { return makeTrack(zoneFt, fps, mph, accel, phase, 0.125); }

static void exact() {
	// Steady 25 mph across 20 ft at 30 fps.
	SpeedTrend s = run(makeTrack(20, 30, 25, 0, 0.3), 20);
	CHECK(s.trend == Trend::Steady);
	CHECK_NEAR(s.accelMphPerSec, 0, 1e-6);
	CHECK_NEAR(s.entryMph, 25, 1e-6);
	CHECK_NEAR(s.exitMph, 25, 1e-6);
	CHECK_NEAR(s.projectedMph, 25, 1e-6);

	// Braking at 8 mph/s from 25 mph: v^2 = v0^2 + 2 a d.
	s = run(makeTrack(20, 30, 25, -8, 0.6), 20);
	double v0 = 25 * kFtPerSecPerMph, a = -8 * kFtPerSecPerMph;
	double vExit = std::sqrt(v0 * v0 + 2 * a * 20), v20 = std::sqrt(vExit * vExit + 2 * a * 20);
	CHECK(s.trend == Trend::Decelerating);
	CHECK_NEAR(s.accelMphPerSec, -8, 1e-6);
	CHECK_NEAR(s.entryMph, 25, 1e-6);
	CHECK_NEAR(s.exitMph, vExit / kFtPerSecPerMph, 1e-6);
	CHECK_NEAR(s.projectedMph, v20 / kFtPerSecPerMph, 1e-6);
	CHECK(s.projectedMph < s.exitMph && s.exitMph < s.entryMph);

	// Hard braking from 12 mph: stops within the next 20 ft, so the projection is 0.
	s = run(makeTrack(14, 60, 12, -9, 0.1), 14);
	CHECK(s.trend == Trend::Decelerating);
	CHECK_NEAR(s.projectedMph, 0, 1e-9);

	// The thresholds: 1.9 steady, 2.1 accelerating, 5.9 accelerating, 6.1 rapid, -2.1 decelerating.
	struct { double a; Trend want; } cases[] = { { 1.9, Trend::Steady }, { 2.1, Trend::Accelerating }, { 5.9, Trend::Accelerating },
		{ 6.1, Trend::RapidlyAccelerating }, { -1.9, Trend::Steady }, { -2.1, Trend::Decelerating } };
	for (auto& c : cases) {
		// No noise and no floor: the class is decided by the thresholds alone.
		Track tr = makeTrack(20, 60, 20, c.a, 0.5);
		SpeedTrend r = speedTrend(tr.t, tr.x, 20, 0, kMaxRmsFt);
		CHECK_NEAR(r.accelMphPerSec, c.a, 1e-6);
		if (r.trend != c.want) std::printf("  a = %.1f gave %s\n", c.a, trendName(r.trend));
		CHECK(r.trend == c.want);
	}
	// A rapid car's projection carries its acceleration on; a steady one's is its exit speed.
	s = run(makeTrack(20, 30, 20, 10, 0.2), 20);
	CHECK(s.trend == Trend::RapidlyAccelerating);
	CHECK(s.projectedMph > s.exitMph + 2);

	// Unknown: too few points, a car not moving forward, a ragged track (two cars merged).
	Track few = makeTrack(20, 30, 25, 0, 0);
	few.t.resize(4);
	few.x.resize(4);
	CHECK(run(few, 20).trend == Trend::Unknown);
	Track back = makeTrack(20, 30, 25, 0, 0);
	for (double& x : back.x) x = -x;
	CHECK(run(back, 20).trend == Trend::Unknown);
	Track ragged = makeTrack(20, 30, 25, 0, 0);
	for (size_t i = 0; i < ragged.x.size(); i += 2) ragged.x[i] += 3;
	CHECK(run(ragged, 20).trend == Trend::Unknown);

	// Names round-trip; Unknown is the empty string.
	for (Trend t : { Trend::Decelerating, Trend::Steady, Trend::Accelerating, Trend::RapidlyAccelerating })
		CHECK(trendFromName(trendName(t)) == t);
	CHECK(trendFromName("") == Trend::Unknown);
	CHECK(trendFromName("rapidly") == Trend::Unknown);
}

struct Rates { double dec = 0, steady = 0, acc = 0, rapid = 0, unknown = 0, mae = 0; };

static Rates simulate(Noise& noise, double zoneFt, double fps, double mph, double accel, double pre, double jitterFt, double floorFt) {
	Rates r;
	const int n = 400;
	for (int k = 0; k < n; k++) {
		Track tr = makeTrack(zoneFt, fps, mph, accel, noise.uniform(), pre);
		for (double& x : tr.x) x += jitterFt * noise.gauss();
		SpeedTrend s = speedTrend(tr.t, tr.x, zoneFt, floorFt, kMaxRmsFt);
		switch (s.trend) {
		case Trend::Decelerating: r.dec++; break;
		case Trend::Steady: r.steady++; break;
		case Trend::Accelerating: r.acc++; break;
		case Trend::RapidlyAccelerating: r.rapid++; break;
		default: r.unknown++; break;
		}
		if (s.known()) r.mae += std::fabs(s.accelMphPerSec - accel);
	}
	double known = n - r.unknown;
	r.mae = known > 0 ? r.mae / known : 0;
	r.dec /= n; r.steady /= n; r.acc /= n; r.rapid /= n; r.unknown /= n;
	return r;
}

// Two kinds of view.  "box": seen from 12% before the zone, 0.1 ft jitter (a generous guess).  "late": what the sample's
// 20 ft short-view crop actually gives, the first front bumper 40% of the way into the zone (the tracker takes a few
// frame pairs to lock on), so only 5-7 points, with 0.03 ft jitter (it strays 0.5-1.5 px rms at 51 px/ft) and the
// engine's 1 px floor.
static void noisy(bool table) {
	Noise noise;
	if (table) std::printf("view zone  fps  mph  accel | decel steady  accel  rapid unknown | mean |a error| mph/s\n");
	for (int late = 0; late < 2; late++)
		for (double zone : { 14.0, 20.0 })
			for (double fps : { 30.0, 60.0 })
				for (double accel : { -8.0, -4.0, 0.0, 4.0, 10.0 }) {
					double mph = accel < 0 ? 22 : 18;   // so the car is at a plausible 15-30 mph throughout
					Rates r = late ? simulate(noise, zone, fps, mph, accel, -0.4, 0.03, 1.0 / 51)
						: simulate(noise, zone, fps, mph, accel, 0.125, kNoiseFt, kFloorFt);
					if (table)
						std::printf("%-4s %4.0f %4.0f %4.0f %+6.0f | %5.0f%% %5.0f%% %5.0f%% %5.0f%% %6.0f%% | %.1f\n", late ? "late" : "box",
							zone, fps, mph, accel, 100 * r.dec, 100 * r.steady, 100 * r.acc, 100 * r.rapid, 100 * r.unknown, r.mae);
					// Never the wrong direction (beyond chance), whatever the view.
					if (accel > 0) CHECK(r.dec <= 0.02);
					if (accel < 0) CHECK(r.acc + r.rapid <= 0.02);
					if (accel == 0) {
						CHECK(r.dec + r.acc + r.rapid <= 0.04);   // false alarms
						CHECK(r.rapid <= 0.01);
					}
					if (zone == 20 && accel == -8) CHECK(r.dec >= 0.8);
					if (zone == 20 && accel == 10) CHECK(r.rapid + r.acc >= 0.9);
					if (zone == 20 && fps == 60 && accel == 4) CHECK(r.acc + r.rapid >= 0.6);
					if (zone == 20 && fps == 60 && accel == 0) CHECK(r.steady >= 0.9);
				}
}

int main(int argc, char** argv) {
	bool table = argc > 1 && std::strcmp(argv[1], "--table") == 0;
	exact();
	noisy(table);
	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
