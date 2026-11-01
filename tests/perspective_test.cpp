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
#include <limits>
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

// --- The camera-setup model behind the ghost car (CameraSetup .. edgeThrough) -------------------------------------------
//
// Together with main()'s checks this takes every line and branch of Perspective.h.  To see it:
//
//   g++ -std=c++17 -O0 --coverage -fno-inline -Isrc/Wintel64/VideoSpeedTracker $(pkg-config --cflags opencv4) \
//       tests/perspective_test.cpp src/Wintel64/VideoSpeedTracker/Globals.cpp $(pkg-config --libs opencv4) -o pt
//   ./pt && gcov -b -c -o . pt-perspective_test.gcda | grep -A4 "Perspective.h'"    (100% lines, 100% branches taken)
//
// The checks are against independent arithmetic: the test's own Cam projector, closed forms for a square
// camera, brute-force bisection for the inverse, and line crossings for the vanishing point.

// Bisection for the X on line (Y, Z) that lands in column u, using only projectPoint: an inverse that shares no algebra
// with streetXAt.  u rises (or falls) monotonically with X on the visible part of the line, [lo, hi].
static double bisectX(const perspective::CameraSetup& c, double u, double Y, double Z, double lo, double hi) {
	auto f = [&](double X) { return perspective::projectPoint(c, 1280, 720, X, Y, Z).u - u; };
	double flo = f(lo);
	for (int i = 0; i < 200; i++) {
		double mid = (lo + hi) / 2, fm = f(mid);
		if ((fm < 0) == (flo < 0)) { lo = mid; flo = fm; } else hi = mid;
	}
	return (lo + hi) / 2;
}

static void cameraModelTests() {
	using namespace perspective;
	const double nan = std::numeric_limits<double>::quiet_NaN();

	// Validity: each limit on its own.
	CameraSetup ok{ 30, 12, 20, 70 };
	CHECK(cameraSetupValid(ok));
	CHECK(!cameraSetupValid({ 0, 12, 20, 70 }));      // no distance
	CHECK(!cameraSetupValid({ 30, -1, 20, 70 }));     // below the road
	CHECK(cameraSetupValid({ 30, 0, 20, 70 }));       // at road level is fine
	CHECK(!cameraSetupValid({ 30, 12, 76, 70 }));     // aimed nearly along the street
	CHECK(!cameraSetupValid({ 30, 12, -76, 70 }));
	CHECK(!cameraSetupValid({ 30, 12, 20, 9 }));      // not a lens
	CHECK(!cameraSetupValid({ 30, 12, 20, 171 }));

	// vanishingFrameX: NaN when square or invalid; otherwise where far-off points along the street land, on the side
	// that is farther (yaw + : right).
	CHECK(std::isnan(vanishingFrameX({ 30, 12, 0, 70 }, 1280)));
	CHECK(std::isnan(vanishingFrameX({ 30, 12, .05, 70 }, 1280)));
	CHECK(std::isnan(vanishingFrameX({ 0, 12, 20, 70 }, 1280)));
	for (double yaw : { 20.0, -20.0, 45.0, -60.0, 5.0 }) {
		CameraSetup c{ 30, 12, yaw, 70 };
		double xV = vanishingFrameX(c, 1280);
		Pixel horizon = projectPoint(c, 1280, 720, yaw > 0 ? 1e11 : -1e11, 12 - 1.1, 30);   // converges as 1/X
		CHECK(horizon.ok);
		NEAR(horizon.u, xV, 1e-3);
		CHECK(yaw > 0 ? xV > 640 : xV < 640);
		// The same point as where two street-parallel lines cross in the picture (the street edge's job).
		auto lineOf = [&](double Y, double Z) {
			Pixel a = projectPoint(c, 1280, 720, -5, Y, Z), b = projectPoint(c, 1280, 720, 5, Y, Z);
			double slope = (b.v - a.v) / (b.u - a.u);
			return Line{ a.v - slope * a.u, a.v + slope * (1280 - a.u), 1280 };
		};
		NEAR(crossingX(lineOf(10.9, 30), lineOf(12, 27)), xV, 1e-6 * std::fabs(xV) + 1e-6);
	}

	// projectPoint: the lane straight down the camera's axis lands at the centre; the test's own projector agrees
	// elsewhere; a point behind the camera, a bad setup or a bad frame size gives ok = false.
	for (double yaw : { 0.0, 25.0, -40.0 }) {
		CameraSetup c{ 30, 12, yaw, 70 };
		double X = 30 * std::tan(yaw * kPi / 180);
		Pixel p = projectPoint(c, 1280, 720, X, 12, 30);
		CHECK(p.ok);
		NEAR(p.u, 640, 1e-9);
		NEAR(p.v, 360, 1e-9);
		Cam ref;
		ref.f = 640 / std::tan(35 * kPi / 180);
		ref.yaw = yaw * kPi / 180;
		ref.pitch = std::atan(12 * std::cos(ref.yaw) / 30);
		ref.D = 30;
		double u, v;
		ref.project(7, 10.9, u, v);
		Pixel q = projectPoint(c, 1280, 720, 7, 10.9, 30);
		NEAR(q.u, u, 1e-9);
		NEAR(q.v, v, 1e-9);
	}
	CHECK(!projectPoint({ 30, 12, 60, 70 }, 1280, 720, -200, 12, 30).ok);   // turned right, a point far to the left
	CHECK(!projectPoint({ 0, 12, 20, 70 }, 1280, 720, 0, 12, 30).ok);
	CHECK(!projectPoint(ok, 0, 720, 0, 12, 30).ok);
	CHECK(!projectPoint(ok, 1280, 0, 0, 12, 30).ok);

	// streetXAt: a round trip through projectPoint, and the same answer as bisection; NaN at the vanishing point, past
	// it (that column shows no point of the line in front of the camera), or for a bad setup.
	for (double yaw : { 0.0, 20.0, -35.0, 60.0 }) {
		CameraSetup c{ 30, 12, yaw, 70 };
		for (double X : { -20.0, -3.0, 0.0, 9.0, 25.0 }) {
			Pixel p = projectPoint(c, 1280, 720, X, 10.9, 27);
			if (!p.ok) continue;
			NEAR(streetXAt(c, 1280, 720, p.u, 10.9, 27), X, 1e-7);
		}
	}
	{
		CameraSetup c{ 30, 12, 20, 70 };
		double u = 900, X = streetXAt(c, 1280, 720, u, 10.9, 27);
		NEAR(X, bisectX(c, u, 10.9, 27, -100, 200), 1e-6);
		double xV = vanishingFrameX(c, 1280);
		CHECK(std::isnan(streetXAt(c, 1280, 720, xV, 10.9, 30)));        // the vanishing point: no single X
		CHECK(std::isnan(streetXAt(c, 1280, 720, xV + 50, 10.9, 30)));   // past it: only points behind the camera
		CHECK(std::isnan(streetXAt({ 30, 12, 20, 5 }, 1280, 720, 640, 10.9, 30)));
	}

	// hubcapLine / zoneFeet.  Square camera: feet = pixels x depth / f, depth = the hubcap line's distance along the
	// tipped-down axis.  Angled: the same feet bisection finds.  A lane too close for the car's near side, or a column
	// past the vanishing point: NaN.
	{
		CarSize car;
		CameraSetup c{ 30, 12, 0, 70 };
		double Y, Z;
		CHECK(hubcapLine(c, car, Y, Z));
		NEAR(Y, 12 - 1.1, 1e-12);
		NEAR(Z, 27, 1e-12);
		double pitch = std::atan(12.0 / 30), f = 640 / std::tan(35 * kPi / 180);
		double depth = std::sin(pitch) * Y + std::cos(pitch) * Z;
		NEAR(zoneFeet(c, 1280, 720, 350, 909), 559 * depth / f, 1e-9);
		NEAR(zoneFeet(c, 1280, 720, 909, 350), 559 * depth / f, 1e-9);   // either order
		CameraSetup a{ 30, 12, 25, 70 };
		NEAR(zoneFeet(a, 1280, 720, 350, 909), bisectX(a, 909, Y, Z, -100, 300) - bisectX(a, 350, Y, Z, -100, 300), 1e-6);
		CHECK(!hubcapLine({ 3.5, 12, 0, 70 }, car, Y, Z));                   // near side half a foot from the wall
		CHECK(std::isnan(zoneFeet({ 3.5, 12, 0, 70 }, 1280, 720, 350, 909)));
		CHECK(std::isnan(zoneFeet(a, 1280, 720, 350, vanishingFrameX(a, 1280) + 10)));
		// A wider lens sees more street between the same columns; a farther lane, too.
		CHECK(zoneFeet({ 30, 12, 0, 90 }, 1280, 720, 350, 909) > zoneFeet(c, 1280, 720, 350, 909));
		CHECK(zoneFeet({ 40, 12, 0, 70 }, 1280, 720, 350, 909) > zoneFeet(c, 1280, 720, 350, 909));
	}

	// ghostCar: its near front hubcap sits on the anchor (the near bottom-front corner straight below it), it points the
	// way it drives, its long edges run to the vanishing point, and a square camera gives exactly f x length / depth.
	{
		CameraSetup c{ 30, 12, 0, 70 };
		Ghost g = ghostCar(c, 1280, 720, 600, 400, true);
		CHECK(g.ok);
		// The near front hubcap (not a corner: 1.1 ft up the front edge) is what lands on (600, 400); the corner below it
		// is the same point lowered by the hubcap height, with the same shift.
		double front = streetXAt(c, 1280, 720, 600, 10.9, 27);
		Pixel hub = projectPoint(c, 1280, 720, front, 10.9, 27), bottom = projectPoint(c, 1280, 720, front, 12, 27);
		NEAR(hub.u, 600, 1e-9);
		NEAR(g.corner[4].u, bottom.u, 1e-9);
		NEAR(g.corner[4].v, bottom.v + (400 - hub.v), 1e-9);
		CHECK(g.corner[4].v > 400);
		CHECK(g.corner[0].u < g.corner[4].u);                  // driving right: the back is to the left
		double pitch = std::atan(12.0 / 30), f = 640 / std::tan(35 * kPi / 180);
		double depth = std::sin(pitch) * 12 + std::cos(pitch) * 27;   // near bottom edge
		NEAR(g.corner[4].u - g.corner[0].u, f * 15.5 / depth, 1e-9);
		CHECK(g.corner[1].v < g.corner[0].v);                  // the roof is above the wheels
		CHECK(g.corner[2].v < g.corner[0].v);                  // the far side is higher up the picture (farther away)
		Ghost left = ghostCar(c, 1280, 720, 600, 400, false);
		CHECK(left.ok && left.corner[0].u > left.corner[4].u);
	}
	for (double yaw : { 20.0, -30.0 }) {
		CameraSetup c{ 30, 12, yaw, 70 };
		Ghost g = ghostCar(c, 1280, 720, 640, 420, yaw > 0);
		CHECK(g.ok);
		double xV = vanishingFrameX(c, 1280);
		for (int k = 0; k < 4; k++) {   // corner k (back) and k + 4 (front) run along the street
			Line edge{ 0, 0, 1280 };
			double slope = (g.corner[k + 4].v - g.corner[k].v) / (g.corner[k + 4].u - g.corner[k].u);
			edge.yLeft = g.corner[k].v - slope * g.corner[k].u;
			edge.yRight = edge.yLeft + slope * 1280;
			Line base{ 0, 0, 1280 };   // the bottom-near edge, the reference
			double s0 = (g.corner[4].v - g.corner[0].v) / (g.corner[4].u - g.corner[0].u);
			base.yLeft = g.corner[0].v - s0 * g.corner[0].u;
			base.yRight = base.yLeft + s0 * 1280;
			if (k) NEAR(crossingX(edge, base), xV, 1e-6 * std::fabs(xV));
		}
	}
	CHECK(!ghostCar({ 3.5, 12, 0, 70 }, 1280, 720, 640, 400, true).ok);                                    // lane too close
	CHECK(!ghostCar({ 30, 12, 20, 70 }, 1280, 720, vanishingFrameX({ 30, 12, 20, 70 }, 1280), 400, true).ok);   // no front
	CHECK(!ghostCar({ 12, 12, 75, 170 }, 1280, 720, 0, 400, true).ok);   // the back of the car would be behind the camera

	// vanishingUsable (the Studio card's "can VST use this" test): no receding is fine; inside the box, behind both
	// lines or receding more than 3x either way is not.
	CHECK(vanishingUsable(nan, 1000, 100, 900));
	CHECK(!vanishingUsable(500, 1000, 100, 900));
	CHECK(!vanishingUsable(-50, 1000, -100, 500));   // between the lines (outside the box): ratio < 0
	CHECK(!vanishingUsable(2000, 1000, 0, 1500));    // right end 4x farther
	CHECK(!vanishingUsable(-100, 1000, 0, 900));     // left end 10x farther
	CHECK(vanishingUsable(3000, 1000, 100, 900));

	// edgeThrough: through the lane line's point at xV from `offset` below; above when below won't fit; halved when
	// neither does; parallel for NaN; refused at xV ~ 0 or when no offset fits.
	{
		Line lane{ 100, 80, 1000 };
		EdgeResult e = edgeThrough(lane, 2500, 40, 200);
		CHECK(e.ok);
		NEAR(e.yLeft, 140, 1e-12);
		NEAR(crossingX(lane, { e.yLeft, e.yRight, 1000 }), 2500, 1e-6);
		e = edgeThrough(lane, -1800, 40, 200);                     // vanishing to the left of the box
		CHECK(e.ok);
		NEAR(crossingX(lane, { e.yLeft, e.yRight, 1000 }), -1800, 1e-6);
		e = edgeThrough(lane, nan, 40, 200);                       // square camera: parallel, 40 below
		CHECK(e.ok);
		NEAR(e.yLeft, 140, 1e-12);
		NEAR(e.yRight, 120, 1e-12);
		e = edgeThrough({ 180, 170, 1000 }, nan, 40, 200);         // too near the bottom: above
		CHECK(e.ok);
		NEAR(e.yLeft, 140, 1e-12);
		e = edgeThrough({ 25, 25, 1000 }, nan, 40, 50);            // neither fits at 40: 20 below
		CHECK(e.ok);
		NEAR(e.yLeft, 45, 1e-12);
		e = edgeThrough({ 25, 25, 1000 }, nan, -40, 50);           // the sign of offset doesn't matter
		CHECK(e.ok);
		NEAR(e.yLeft, 45, 1e-12);
		e = edgeThrough({ 180, 5, 1000 }, nan, 40, 200);           // below: left end off the box; above: right end
		CHECK(e.ok);                                               // off the top; so 20 below
		NEAR(e.yLeft, 200, 1e-12);
		e = edgeThrough({ 5, 180, 1000 }, nan, 40, 200);           // below: right end off the bottom; above: left end
		CHECK(e.ok);                                               // off the top; 20 below
		NEAR(e.yRight, 200, 1e-12);
		CHECK(!edgeThrough({ 1, 1, 1000 }, nan, 40, 1).ok);        // a one-row box: nothing fits
		CHECK(!edgeThrough(lane, .5, 40, 200).ok);                 // on the box's left edge
		CHECK(!edgeThrough(lane, -.5, 40, 200).ok);
		CHECK(!edgeThrough(lane, 2500, .5, 200).ok);               // no offset at all: no line apart from the lane
	}
}

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

	cameraModelTests();

	std::printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
