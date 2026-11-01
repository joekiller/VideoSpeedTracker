
//                  Copyright Paul Reynolds, Locust Avenue, Charlottesville, Va,  2016
//                                     All rights reserved.

//                                     License Agreement
//                                For VideoSpeedTracker (VST)
//                                 (3 - clause BSD License)

// Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following
// conditions are met :

// 1) Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
// 2) Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
//      in the documentation and / or other materials provided with the distribution.
// 3) Neither the name of the copyright holder nor the names of the contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.

// This software is provided by the copyright holder and contributors "as is" and any express or implied warranties, including,
// but not limited to, the implied warranties of merchantability and fitness for a particular purpose are disclaimed.In no event
// shall copyright holders or contributors be liable for any direct, indirect, incidental, special, exemplary, or consequential
// damages(including, but not limited to, procurement of substitute goods or services; loss of use, data, or profits; or business
// interruption) however caused and on any theory of liability, whether in contract, strict liability, or tort(including negligence
// or otherwise) arising in any way out ofthe use of this software, even if advised of the possibility of such damage.

#pragma once

#define MYLIB_CONSTANTS_H 1
#define CVBlue 255,0,0
#define CVGreen 0,255,0
#define CVRed 0,0,255
#define CVYellow 0,255,255
#define CVOrange 0,128,255
#define CVPurple 255,0,127
#define CVCyan 255,255,153
#define CVPink 178,102,153
#define CVWhite 255,255,255
#define CVBlack 0,0,0

#include "Perspective.h"
#include <algorithm>
#include <string>
#include <vector>

enum direction { L2R, R2L, UNK };
enum vehicleStatus { entering, inMiddle, exiting, exited };
enum statusTypes { ImOK, deleteWithStats, lostTrack, negVelocity };
enum OverlapType { none, rearOnly, frontOnly, bothOverlap };
enum grabType { greedy, strict };

using namespace std;



class Globals
{
public:
	Globals();

	~Globals();

	bool readConfig(const string& path = "VST.cfg");

	// Everything wrong with the current values (analysis box off the frame, speed lines out of order, ...), one
	// message per problem; empty when the settings are usable.  readConfig() reports these after reading.
	// Also resolves a compass LegalDirection ("NE") to L2R or R2L, so call it after changing the directions.
	vector<string> problems();

	// True when a vehicle travelling this way is going against LegalDirection on a one-way street.
	bool isWrongWay(bool leftToRight) const {
		return (leftToRight && LegalDirection == "R2L") || (!leftToRight && LegalDirection == "L2R");
	}

	// VST works in 1280x720 frame coordinates; every config pixel value refers to them.  Input video of any
	// other size is resized to this before analysis (see main() in videoSpeedTracker.cpp).
	static constexpr int FrameWidth = 1280;
	static constexpr int FrameHeight = 720;
	double videoFPS = 30.0;			// Frame rate of the video being analyzed; set by main() from the input, not from the config.

	const int pixelLeft = 0;				// This won't change.  It's used relative to AnalysisBoxLeft.
	int pixelRight = 1279;                  // This will change to be AnalysisBoxWidth.  Change should happen after AnalysisBoxLeft and Width are read in from config.

	// * * * * * * * * v v v v * * * * * * * * R e a d i n g   o f   C o n f i g    f i l e   s h o u l d    o v e r r i d e   t h e s e * * v v v v * * * * * * //

	string dataPathPrefix;						// path to data directories, IPCam, Stats, etc.  Empty = userpaths::dataDir().  Don't use double backslash in config file!.
	string L2RDirection = "SE";					// Compass Direction L2R vehicles are heading
	string R2LDirection = "NW";					// Compass Direction R2L vehicles are heading
	string LegalDirection = "Both";				// One-way streets: L2R or R2L (or the compass name of one, e.g. NE); the other way is logged as wrong-way.  Both = two-way.
	int obstruction[2] = { 0, 0 };	// left / right x coords bounding vertical obstructions in foreground.Max 2. Relative to AnalysisBoxLeft. ...pixels
	int AnalysisBoxLeft = 10;		// How much to crop off left side of full frame for analysis box. Want to keep this small(zero!) if possible
	int AnalysisBoxTop = 220;		// How much to crop off top of full frame to get rid of tree tops, across - the - street houses, etc.
	int AnalysisBoxWidth = 1269;	// Width of Analysis Box relative to AnalysisBoxLeft.Their sum must be <= 1279. ...pixels
	int AnalysisBoxHeight = 190;	// measured down from Analysis Box Top; determines height of Analysis Box.  (Top + Height) <= 719.
	int speedLineLeft = 350;		// x coord of left white line for speed measuring box, realtive to AnalysisBoxLeft
	int speedLineRight = 909;		// x coord of right white line for speed measuring box, realtive to AnalysisBoxLeft
	int maxL2RDistOnEntry = 75;		//  Maximum trackable speed, delta pixels per two frames.  Represents about 70MPH;
	int maxR2LDistOnEntry = 75;		//  Maximum trackable speed, delta pixels per two frames.  Represents about 70MPH;
	int entryLookBack = 250;		//  Use this to force looking back as vehicle enters Analysis Box.
	int obstruction_extent = 30;	// Even after a vehicle has passed an obstruction it needs space to show up as a blob past the obstruction in differencing operation.
	int largeVehicleArea = 109;		// After dividing a computed area by 300.  This value tends to separate buses and UPS trucks from large pickups.
	int CalibrationFramesL2R = 35;	// Measured three times on 14 Jan 2016 for Locust Avenue, Charlottesville, VA
	int CalibrationFramesR2L = 40;	// Measured three on 14 Jan 2016 for Locust Avenue, Charlottesville, VA
	double CalibrationSpeed = 25.0;	// MPH the calibration vehicle drove when CalibrationFramesL2R/R2L were counted.  Speeds scale from it.
	double CalibrationFPS = 30.0;	// Frame rate of the video the calibration frames were counted in, so other frame rates measure correctly.
	int SENSITIVITY_VALUE = 30;		// Sensitivity value for the OpenCV absdiff funtion. Change with care.
	int	BLUR_SIZE = 20;				// To smooth the intensity image output from absdiff() function. Change with care.
	int	SLOP = 15;					// Margin of error when testing for vehicle overlap... pixels.
	int R2LStreetY = 122;			// Hubcap line for R2L vehicles on flat street.Orange. Locust Ave.  Relative to AnalysisBoxTop...pixels
	int L2RStreetY = 158;			// Hubcap line for L2R vehicles on flat street.Purple. Locust Ave. Relative to AnalysisBoxTop...pixels
	int R2LStreetYRight = 0;		// The R2L hubcap line's height at the analysis box's right edge, for a street that runs at an angle across the picture (Perspective.h).  0 = same as R2LStreetY (level).
	int L2RStreetYRight = 0;		// The same for the L2R line.  0 = same as L2RStreetY.
	int streetEdge[2] = { 0, 0 };	// Optional second line along the street (curb, lane stripe), at the box's left and right edges, relative to AnalysisBoxTop.  Where it
									// converges with a lane line is the street's vanishing point, which short-view timing uses to measure along the ground (Perspective.h).  [0,0] = off.
	int nextHeight = 85;			// Initial best guess for height of entering vehicles...pixels.
	int minLoggedSpeed = 18;		// MPH.  Vehicles timed slower than this (walkers, cyclists, cars turning in) are counted in the run manifest's belowMinSpeed but not written to the csv.  0 = log every timed vehicle.  18 was hard-coded until v1.2.
	static constexpr double kMaxLevelDegrees = 15;  // levelDegrees limit: past this, re-aim the camera instead
	double levelDegrees = 0;		// Camera roll correction: every frame is turned this many degrees counter-clockwise about its centre (Level.h) before
									// anything else sees it, so a camera that isn't mounted level still puts each lane on one row.  0 = off.
	int subFrameTiming = 0;			// 1 for a short view (about 14-25 ft of street, a car filling most of the frame): time the front bumper to a fraction of a frame and scale the tracker's pixel margins (see margin()).  0 = whole frames, as before v1.3.

	// The hubcap line for one direction (relative to AnalysisBoxTop) at x (relative to AnalysisBoxLeft); its left and right ends, and the lowest row it
	// reaches, below which nothing is looked at for that direction.
	perspective::Line lane(bool leftToRight) const {
		int left = leftToRight ? L2RStreetY : R2LStreetY, right = leftToRight ? L2RStreetYRight : R2LStreetYRight;
		return { double(left), double(right > 0 ? right : left), double(std::max(AnalysisBoxWidth, 1)) };
	}
	int laneBottom(bool leftToRight) const {
		perspective::Line l = lane(leftToRight);
		return int(std::ceil(std::max(l.yLeft, l.yRight)));
	}
	bool laneTilted(bool leftToRight) const { perspective::Line l = lane(leftToRight); return l.yLeft != l.yRight; }
	bool hasStreetEdge() const { return streetEdge[0] > 0 && streetEdge[1] > 0; }
	// The street's vanishing point as an x relative to AnalysisBoxLeft, from where streetEdge meets this direction's lane line; NaN when streetEdge is
	// off or (nearly) parallel to the lane line, i.e. no receding to correct for.
	double vanishingX(bool leftToRight) const {
		if (!hasStreetEdge()) return std::numeric_limits<double>::quiet_NaN();
		perspective::Line edge{ double(streetEdge[0]), double(streetEdge[1]), double(std::max(AnalysisBoxWidth, 1)) };
		return perspective::crossingX(lane(leftToRight), edge);
	}
	// Distance along the street from the left speed line (0) to the right one (1) for a front bumper at x, measured on the ground (Perspective.h).
	double zoneFraction(bool leftToRight, double x) const {
		return perspective::zoneFraction(x, speedLineLeft, speedLineRight, vanishingX(leftToRight));
	}

	// Rails for the speed zone, shared by every place that edits it (VST Studio's canvas and fields, the companion's
	// editor via PUT /api/config, and problems() when VST.cfg loads):
	//   - the speed lines sit inside the analysis box, left before right, at least kMinZone apart, so a car is seen at
	//     several frame pairs between them (fewer, and the bumper fit has nothing to fit);
	//   - so the box is at least kMinZone wide.
	// No room is needed between a line and the box's side: the tracker times a car that comes into view already at or
	// over a line on the box's edge (videoSpeedTracker.cpp entryPosition(), VehicleDynamics::markEnteredAtEdge()), and
	// on the 2016 sample a left line 0, 20, 50 or 100 px in from the box's side times the same 25 of 28 entering cars.
	static constexpr int kMinZone = 200;
	// Puts the speed lines back on the rails after an edit, moving the one that wasn't edited: keepLeft = true when the
	// left line (or the box's left side) is what the user just moved.  Needs AnalysisBoxWidth >= kMinZone.
	void railZone(bool keepLeft) {
		const int w = AnalysisBoxWidth;
		if (keepLeft) {
			speedLineLeft = std::clamp(speedLineLeft, 0, std::max(w - kMinZone, 0));
			speedLineRight = std::clamp(speedLineRight, std::min(speedLineLeft + kMinZone, w), w);
		}
		else {
			speedLineRight = std::clamp(speedLineRight, std::min(kMinZone, w), w);
			speedLineLeft = std::clamp(speedLineLeft, 0, std::max(speedLineRight - kMinZone, 0));
		}
	}

	// A pixel margin tuned for the 2016 Locust Avenue view, where maxL2RDistOnEntry and maxR2LDistOnEntry are 75.  With
	// subFrameTiming it grows in proportion to that direction's maxDistOnEntry, so a short view, where a car moves hundreds of
	// pixels a frame pair, keeps the same margins relative to the motion.  Without subFrameTiming it is returned unchanged.
	int margin(int pixels, bool leftToRight) const {
		if (!subFrameTiming) return pixels;
		return pixels * (leftToRight ? maxL2RDistOnEntry : maxR2LDistOnEntry) / 75;
	}

private:


};
