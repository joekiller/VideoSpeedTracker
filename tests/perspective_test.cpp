// Angled lane lines and a receding street (src/Wintel64/VideoSpeedTracker/Perspective.h and the Globals helpers built
// on it).
//
//   perspective_test
//
// The core claim: with the street's vanishing point taken from two lines that are parallel on the ground, zoneFraction()
// turns a bumper's pixel x into distance along the street, so equal steps on the ground (a car at a steady speed, frame
// by frame) come out as equal steps.  Checked against a pinhole camera looking along a street at several angles, where
// the true ground positions are known.  Also: the plain linear map when there's no street edge, the depth ratio the
// Studio card shows, the lane helpers (level lines unchanged, angled lines, laneBottom), and problems() refusing a
// street edge that crosses the lane inside the box or recedes too far.
#include "Globals.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

static const double kPi = 3.14159265358979323846;

// A pinhole camera at the origin, focal length f px, image centre (640, 360), turned `yawDeg` from facing the street
// squarely and looking down `pitchDeg`; the street runs along world X at distance D in front (Z) and the line is at
// height h below the camera (world Y down).  Returns the image point of ground point X.
struct Cam {
	double f = 900, yaw = 0, pitch = 0, D = 30;
	void project(double X, double h, double& u, double& v) const {
		// World point, then rotate into the camera: yaw about Y, then pitch about X.
		double x = X, y = h, z = D;
		double cy = std::cos(yaw), sy = std::sin(yaw);
		double x1 = cy * x - sy * z, z1 = sy * x + cy * z;
		double cp = std::cos(pitch), sp = std::sin(pitch);
		double y2 = cp * y - sp * z1, z2 = sp * y + cp * z1;
		u = 640 + f * x1 / z2;
		v = 360 + f * y2 / z2;
	}
};

int main() {
	using namespace perspective;

	// No vanishing point: the old linear map.
	NEAR(zoneFraction(350, 350, 909, NAN), 0, 1e-12);
	NEAR(zoneFraction(909, 350, 909, NAN), 1, 1e-12);
	NEAR(zoneFraction(629.5, 350, 909, NAN), .5, 1e-12);
	NEAR(depthRatio(350, 909, NAN), 1, 1e-12);
	CHECK(std::isnan(crossingX({ 100, 120, 1000 }, { 150, 170, 1000 })));  // parallel
	NEAR(crossingX({ 100, 100, 1000 }, { 200, 150, 1000 }), 2000, 1e-9);     // close in 50 px per 1000: meet at 2000

	// A street seen at an angle: equal ground steps must give equal zoneFraction steps, and depthRatio must be the true
	// ratio of distances along the camera axis at the two speed lines.
	for (double yawDeg : { 10.0, 25.0, -20.0, 0.0 }) {
		Cam cam;
		cam.yaw = yawDeg * kPi / 180;
		cam.pitch = 12 * kPi / 180;
		// The lane line (hubcap height, 2 ft above the road) and the curb (on the road, 3 ft nearer), as a box from x 0
		// to 1280 would see them: each line's rows at the box's left and right edges.  Lines stay straight in the picture,
		// so two projected ground points each define them.
		const double camH = 15;  // camera 15 ft up
		auto uAt = [&](double X, double h) { double u, v; cam.project(X, h, u, v); return u; };
		auto rowAt = [&](double u, double h, double dz) {
			Cam c2 = cam; c2.D = cam.D + dz;
			double u0, v0, u1, v1;
			c2.project(-10, h, u0, v0); c2.project(10, h, u1, v1);
			return v0 + (v1 - v0) * (u - u0) / (u1 - u0);
		};
		Line lane{ rowAt(0, camH - 2, 0), rowAt(1280, camH - 2, 0), 1280 };
		Line curb{ rowAt(0, camH, -3), rowAt(1280, camH, -3), 1280 };
		double xV = crossingX(lane, curb);
		// Speed lines where ground X = -8 and +8 ft cross the lane line.
		double xL = uAt(-8, camH - 2), xR = uAt(8, camH - 2);
		if (xL > xR) std::swap(xL, xR);
		if (yawDeg == 0) { CHECK(std::isnan(xV)); continue; }   // squarely across: no receding
		CHECK(!std::isnan(xV));
		CHECK(vanishingUsable(xV, 1280, xL, xR));
		// Ground X from -8 to 8 in 1 ft steps: zoneFraction must step evenly by 1/16 (to 1e-6 of the zone).
		double maxErr = 0;
		bool leftToRight = uAt(8, camH - 2) > uAt(-8, camH - 2);
		for (int i = 0; i <= 16; i++) {
			double X = -8 + i, u = uAt(X, camH - 2);
			double s = zoneFraction(u, xL, xR, xV), want = leftToRight ? i / 16.0 : 1 - i / 16.0;
			maxErr = std::max(maxErr, std::fabs(s - want));
		}
		std::printf("yaw %+5.1f deg: vanishing x %.0f, depth ratio %.3f, max zoneFraction error %.2e\n", yawDeg, xV, depthRatio(xL, xR, xV), maxErr);
		CHECK(maxErr < 1e-4);
		// The plain pixel map is clearly worse here, or this test proves nothing.
		double linErr = 0;
		for (int i = 0; i <= 16; i++) {
			double u = uAt(-8 + i, camH - 2), want = leftToRight ? i / 16.0 : 1 - i / 16.0;
			linErr = std::max(linErr, std::fabs(zoneFraction(u, xL, xR, NAN) - want));
		}
		CHECK(linErr > 10 * maxErr + 1e-3);
		// Depth ratio: camera-axis depth of the two ends.
		auto depth = [&](double X) {
			double x = X, z = cam.D, cy = std::cos(cam.yaw), sy = std::sin(cam.yaw);
			double z1 = sy * x + cy * z, cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
			return sp * (camH - 2) + cp * z1;
		};
		double dLeft = depth(leftToRight ? -8 : 8), dRight = depth(leftToRight ? 8 : -8);
		NEAR(depthRatio(xL, xR, xV), dRight / dLeft, 1e-4);
	}

	// Globals: a level lane line is what it always was.
	Globals g;
	CHECK(!g.laneTilted(true) && !g.laneTilted(false));
	CHECK(g.laneBottom(false) == g.R2LStreetY);
	NEAR(g.lane(true).at(500), g.L2RStreetY, 1e-12);
	CHECK(std::isnan(g.vanishingX(true)));
	NEAR(g.zoneFraction(true, g.speedLineLeft), 0, 1e-12);
	NEAR(g.zoneFraction(true, g.speedLineRight), 1, 1e-12);
	CHECK(g.problems().empty());

	// Angled: R2L from 122 at the left edge to 100 at the right.
	g.R2LStreetYRight = 100;
	CHECK(g.laneTilted(false) && !g.laneTilted(true));
	CHECK(g.laneBottom(false) == 122);
	NEAR(g.lane(false).at(g.AnalysisBoxWidth / 2.0), 111, 1e-9);
	CHECK(g.problems().empty());
	g.R2LStreetYRight = g.AnalysisBoxHeight + 1;
	CHECK(!g.problems().empty());
	g.R2LStreetYRight = 100;

	// Street edge closing in on the lane lines to the right (vanishing point well right of the box): usable.  On a
	// two-way street it is checked against both lanes, so the L2R line gets the same slant.
	g.L2RStreetY = 150; g.L2RStreetYRight = 128;
	g.streetEdge[0] = 185; g.streetEdge[1] = 155;   // R2L gap 63 at the left, 55 at the right: they meet about 8 box widths out
	double xV = g.vanishingX(false);
	CHECK(xV > g.AnalysisBoxWidth);
	CHECK(g.problems().empty());
	double r = depthRatio(g.speedLineLeft, g.speedLineRight, xV);
	CHECK(r > 1.02 && r < 1.2);
	// Crossing the lane line inside the box: refused, and the message names the setting.
	g.streetEdge[0] = 170; g.streetEdge[1] = 60;
	{
		auto p = g.problems();
		CHECK(p.size() == 1 && p[0].find("streetEdge") != std::string::npos);
	}
	// Half set, or off the box: refused.  [0,0]: off, fine.
	g.streetEdge[0] = 0; g.streetEdge[1] = 150;
	CHECK(!g.problems().empty());
	g.streetEdge[0] = g.streetEdge[1] = 0;
	CHECK(g.problems().empty());

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
