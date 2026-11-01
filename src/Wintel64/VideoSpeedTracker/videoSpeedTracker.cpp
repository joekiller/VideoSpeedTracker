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

// ================
//Thank you Kyle Hounslow for your helpful Youtube videos on motion tracking in B/W
// ================

//***********************************************************************************************************************
//   This program processes video images of a typical two lane bidirectional street, tracks vehicles passing along that street,
// and forms an estimate of each vehicle's speed.  Speadsheet data, in the form of a csv file, is produced by this porgram.  Each
// entry in the csv file captures vehicle direction, frames in which the vehicle was analyzed for speed, the vehicle's profile area,
// and the vehicle's estimated speed. 
//   This program identifies vehicles in a video stream using what is classically known as "frame differencing", wherein
// two consecutive video frames are differenced, against a relatively static background, in order to detect where motion has
// occurred.  Often the shape arising in this differencing method approximates the shape of the vehicle.  More importantly, the
// leading and trailing edges of the vehicle can generally be discerned.  A sequence of frame differences can be used to approximate
// vehicle position over time, and therefore its velocity.
//   Frame differencing can be noisy.  Glare off of windshields, shadows, similarly colored objects in the background and other artifacts
// can make validity of a single frame difference questionable.  However, over time, most sequences of frame differences will produce
// sufficient useful information to allow for reliable tracking of motion.  As is done in most radar systems, tracking reliability can
// be enhanced significantly through the employment of a predictive tracking filter.  VST uses such a method to separately track
// both the front edge of a vehicle and its trailing edge, and to predict where useful data about the vehicle will appear in a
// subsequent frame difference.  The filtering/tracking method used in VST is a piecewise linear least squares method, which has
// proven reliable under many circumstances in many tracking applications.
//   VST is not perfect.  Mistracking can occur in the event frame difference data is too noisy to produce useful tracking, and therefore
// speed estimation information.  VST produces an optional highlights video, which is intended to be used as a quality checker.  Before
// making any results from VST public, the highlights video should be inspected manually for quality.
//  A feature of VST that I have not seen in similar tools in the public domain before is handling of bidirectional traffic.  VST predicts
// when two vehicles will pass and engages dead-reckoning to move the vehicle through conditions where raw data (frame difference images)
// yield little useful data (without significant image shape analysis, which itself can be easily fooled by noisy data).  Dead-reckoning
// of passing vehicles is working quite well in VST, based on my own observations of its ability to maintain track on the front bumper
// of vehicle as it passes another.  Your experience may vary depending on conditions I can't control.
//*****************************************************************************************************************************


#include <opencv2/opencv.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry.hpp>  // OpenCV 5 moved boundingRect() out of imgproc
#endif
#include "Globals.h"
#include <iostream>
#include <fstream>
#include <queue>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <chrono>
#include <thread>
#include <ctime>
#include <cmath>
#include <sstream>
#include <locale>
#include <opencv2/videoio/registry.hpp>
#include "VehicleDynamics.h"
#include "Projection.h"
#include "Snapshot.h"
#include "UserPaths.h"
#include "Engine.h"
#include "PiCamera.h"
#include "FrameInput.h"
#include "Level.h"

#ifndef VST_VERSION
#define VST_VERSION "dev"  // CMake stamps the release version in; it goes into the run manifest's "vst"
#endif



using namespace std;
using namespace cv;
namespace fs = std::filesystem;

const int MAX_NUM_OBJECTS = 30; // Max number of objects allowed to be retunred by contours
const int MIN_OBJECT_AREA = 30 * 35;  // Very sensitive to pedestrians, bicyclists and other small things.
int numObjects = 0;  // 
Rect coalescedRectangle;  //  The collection of blobs that represent a vehicles projected area.

vector<VehicleDynamics> vehiclesGoingRight;
vector<VehicleDynamics> vehiclesGoingLeft;

bool bailing = false;



// ........................................................ Globals shared between setup() and main() ................................................
ofstream traceFile;
ofstream statsFile;
unique_ptr<frameio::FrameSource> capture = frameio::closed();  // the source being read: never null (FrameInput.h port; frameio::open picks the adapter)
vector<string> filesToDo;  // Full paths of the videos main() processes, in order.  One entry unless "*" was chosen in interactive mode.
string fileName;  // Name (no directory) of the video currently being processed.  Foscam names "manual_yyyymmddhhmmss.avi" supply the csv Date/Time columns (see setTimeBase()).
int objDelay = 50;  // ms per frame pair while a vehicle is on screen (about real time).  "s" slows it to 250 then 1250 ms for close inspection, "f" speeds it up.  Was 1250; 50 is from piki's 2017 fork.
bool pleaseTrace = false;  // If you want a trace file (lots of debug info)
bool highLightsPlease = false;
int speedLimit = 25;  // User supplied speed limit, used for color choice when posting speed
int egregiousSpeedLowerBound = 35;    // User supplied egregious speed lower bound, used for color choice when posting speed
int crazySpeed = 55;
int highLightsSpeedLower = 35; // Default lower threshold for including vehicles in the highlights file
int highLightsSpeedUpper = 100; // Default upper threshold for including vehicles in the highlights file
int minimumProfileArea = 100;  // Default lower bound on size of large vehicle to be added to highlights if speeding over speed limit.
int frameNumber; // Current framenumber being processed, relative to beginning of file "fileName"
double startFrame = 0.0;
Rect AnalysisBox;  // the coordinates and extents of the region beng analyzed for vehicle motion.  Subregion of frames read in.
VideoWriter hiLiteVideo; // For writing highlights...the Scofflaws
Globals g;

Mat frame1, frame2; // Frames read by main, to use in frame differencing, and for display.

// Command-line mode (any arguments given): no prompts, no Windows "dir" calls, optional headless run with no GUI windows.
bool headless = false;    // --headless: never open a window or call waitKey (servers, containers, cron jobs)
string inputPath;         // --input: full path of the single video to analyze; empty means interactive mode
string configPath;        // --config; empty = userpaths::resolveConfig("VST.cfg")
bool demo = false;        // --demo: analyze the bundled sample video with the settings it was recorded with

#ifdef VST_EMBEDDED
// Set by vst::run() for the length of one run.  frameBoxes collects what displayAnalysisGoing*() drew this frame pair.
const vst::Hooks* hooks = nullptr;
vector<vst::TrackBox> frameBoxes;
struct EngineExit { int code; };  // thrown by quit() so an embedded run unwinds back to vst::run() instead of ending the process
#endif

// exit() for the command-line build; an embedded run returns the status from vst::run() instead.
[[noreturn]] void quit(int code){
#ifdef VST_EMBEDDED
	throw EngineExit{ code };
#else
	exit(code);
#endif
}
//......................................................................................................................................................


// One row per logged vehicle.  Flag is "*****" for an implausible speed, else empty; it used to be written only when set, and is
// now always there so that Timestamp (ISO 8601 local time with UTC offset, empty when unknown) is always the 15th column.
// The speed trend follows (v1.10+, SpeedTrend.h): EntryMph and ExitMph at the two speed lines, AccelMphPerSec (+ speeding
// up), Trend (decelerating, steady, accelerating or rapid) and MphIn20ft, the speed projected 20 ft past the exit line.
// All five are empty when the trend can't be told.  Readers of the older 15 columns ignore them.
const string statsHeader = "Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp, "
	"EntryMph, ExitMph, AccelMphPerSec, Trend, MphIn20ft";

// The five speed-trend cells of a stats row (see statsHeader), "" in each when unknown.
string trendCells(const vst::SpeedTrend& t){
	if (!t.known()) return ", , , , ";
	char buf[128];
	snprintf(buf, sizeof buf, "%d, %d, %.1f, %s, %d", int(t.entryMph + 0.5), int(t.exitMph + 0.5), t.accelMphPerSec,
		vst::trendName(t.trend), int(t.projectedMph + 0.5));
	return buf;
}

// Reads the next frame and scales it to the 1280x720 canvas every config coordinate refers to, so cameras that record
// 1080p, 4K or 640x480 work with the same VST.cfg geometry, then levels it (levelDegrees).  Returns false at end of video or on a read error.
// The working frame is made by the frame input port (FrameInput.h), the same way VST Studio's preview makes it.
static_assert(Globals::FrameWidth == frameio::kWorkingWidth && Globals::FrameHeight == frameio::kWorkingHeight, "the frame port's working size is VST.cfg's canvas");
bool readFrame(frameio::FrameSource& cap, Mat& frame){
	return frameio::readWorking(cap, frame, g.levelDegrees);
}

// What kind of source a name is: frameio::classify() decides for the engine and VST Studio alike.  An all-digit name like
// "0" is that local camera; "picam", "picam:1", "picam:0?fps=60&shutter=1000" a Raspberry Pi ribbon camera (PiCamera.h).
bool isCameraIndex(const string& name){ return frameio::classify(name) == frameio::Kind::Camera; }
bool isPiCamera(const string& name){ return frameio::classify(name) == frameio::Kind::PiCamera; }

// A camera of either kind: always live, and named "camera" in the run manifest.
bool isLiveCamera(const string& name){ return frameio::isCamera(frameio::classify(name)); }

bool isStreamUrl(const string& name){ return frameio::classify(name) == frameio::Kind::Stream; }

// Opens a video file, a stream URL (rtsp://, http://, anything FFmpeg reads), for an all-digit name like "0" that local
// camera, or for "picam..." a Raspberry Pi camera, through the frame input port: frameio::open() picks the adapter and
// applies the opening policy (hardware decoding, USB cameras at 1280x720 MJPG 60 fps, stream timeouts; see FrameInput.h).
// A Pi camera that can't start says why on stderr (once per distinct reason, so a reconnect loop doesn't repeat it every
// attempt), and once it starts, its settings go in the log.
string lastPiError;  // cleared before each video's first open, so every run reports its own reason
bool openSource(unique_ptr<frameio::FrameSource>& cap, const string& name){
	cap->release();  // one reader per camera: let go of the old one before the new one asks for it
	cap = frameio::open(name);
	bool ok = cap->isOpened();
	if (isPiCamera(name)){
		if (!ok && cap->error() != lastPiError) cerr << cap->error() << endl;
		lastPiError = ok ? "" : cap->error();
	}
	if (ok) for (const string& note : cap->notes()) cout << note << endl;
	return ok;
}

// A source name that is safe to write down (the log, the run manifest): frameio::safeName() (FrameInput.h) drops a URL's
// user:password@ and any query parameter that looks like a password (Foscam and many IP cameras take ?user=admin&pwd=secret).
// File paths and camera indexes come back as they are.
string withoutPassword(const string& name){ return frameio::safeName(name); }

// A source for the log: "camera 0", "Pi camera 0", or the file or URL without any password it carries.
string sourceLabel(const string& name){ return frameio::label(name); }

// Output name for a video: Foscam "manual_20160130172251.avi" -> "20160130_172251" (the 2016 naming), anything else -> its stem.
string outputStem(const string& videoName){
	string stem = fs::path(videoName).stem().string();
	bool foscam = stem.size() == 21 && stem.compare(0, 7, "manual_") == 0
		&& all_of(stem.begin() + 7, stem.end(), [](char c){ return isdigit((unsigned char)c) != 0; });
	return foscam ? stem.substr(7, 8) + "_" + stem.substr(15, 6) : stem;
}

// ............................................ When each vehicle passed: the csv Date, Time and Timestamp columns ............................................
// A report for a city council has to say when each vehicle went by, not just which frame of which video it was in.  Every
// logged vehicle gets a time from the best clock available for its source:
//   - live sources (camera index, Pi camera or stream URL): this computer's clock at the moment the vehicle is logged;
//   - Foscam recordings (manual_yyyymmddhhmmss.avi): the start time in the name plus the vehicle's frame offset;
//   - other recordings: the file's modification time (when the camera finished writing it) minus the video's length,
//     plus the frame offset.  That fails for copies (copying resets the time), so it's only used when it looks sane.
// When none of these can be trusted the time is left blank rather than guessed; the Timestamp column is still written, empty.
// Timestamp is ISO 8601 local time with its UTC offset ("2026-10-30T14:03:22-04:00"), so it stays unambiguous across
// daylight-saving changes and when the csv is read somewhere else.  Date and Time ("20261030", "140322") are the same
// moment, except for Foscam names, where they keep their 2016 meaning: when the recording started (the same on every row).
bool liveSource = false;       // the current source is a camera or stream: times come from the clock, and dropouts are reconnected
double recordingStart = -1;    // files: seconds since 1970 (UTC) at frame 0 of the current file; -1 when unknown
string foscamDateTime;         // "yyyymmdd, hhmmss" from a Foscam name, else empty

// Local calendar fields of t, and its UTC offset in minutes.  The offset is the difference between the local fields read as
// if they were UTC (days_from_civil, after H. Hinnant) and t itself; strftime's %z isn't "+hh:mm" and varies on Windows.
tm localFields(time_t t, int& offsetMinutes){
	tm lt{};
#ifdef _WIN32
	localtime_s(&lt, &t);
#else
	localtime_r(&t, &lt);
#endif
	long long y = lt.tm_year + 1900, m = lt.tm_mon + 1, d = lt.tm_mday;
	y -= m <= 2;
	long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
	long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
	long long localSeconds = days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
	offsetMinutes = int((localSeconds - (long long)t) / 60);
	return lt;
}

// t (seconds since 1970) as "2026-10-30T14:03:22-04:00", or as the csv's "20261030, 140322" when csvColumns.
string formatTime(double t, bool csvColumns){
	int off = 0;
	tm lt = localFields(time_t(floor(t)), off);
	char s[48];
	if (csvColumns)
		snprintf(s, sizeof s, "%04d%02d%02d, %02d%02d%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec);
	else
		snprintf(s, sizeof s, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min, lt.tm_sec,
			off < 0 ? '-' : '+', abs(off) / 60, abs(off) % 60);
	return s;
}

double clockNow(){ return chrono::duration<double>(chrono::system_clock::now().time_since_epoch()).count(); }

// Seconds since 1970 when frame `frame` of the current source was taken, or -1 if unknown: this computer's clock for a live
// source (so call it as the frame arrives), else the recording's start plus the frame's offset into it.
double frameTime(int frame){
	if (liveSource) return clockNow();
	return recordingStart >= 0 ? recordingStart + frame / g.videoFPS : -1;
}

// Seconds since 1970 when the vehicle being logged now (at frameNumber) passed, or -1 if unknown.
double vehicleTime(){ return frameTime(frameNumber); }

// csv Date and Time columns, e.g. "20160130, 172251"; blank ", " when the time is unknown.
string csvDateTime(double when){
	if (!foscamDateTime.empty()) return foscamDateTime;
	return when >= 0 ? formatTime(when, true) : string(", ");
}

// Works out recordingStart for the file just opened in capture (see above), and says in the log which clock the csv times
// rest on, so whoever builds a report from them later knows how far to trust them.
void setTimeBase(const string& source, bool fpsReported){
	recordingStart = -1;
	foscamDateTime.clear();
	if (liveSource){
		cout << "Vehicle times: this computer's clock (live source)." << endl;
		return;
	}
	string stem = outputStem(fileName);  // "yyyymmdd_hhmmss" for a Foscam name, else the plain stem
	if (stem != fs::path(fileName).stem().string()){
		foscamDateTime = stem.substr(0, 8) + ", " + stem.substr(9, 6);
		// The name is the camera's local time; read it as this computer's local time (mktime works out daylight saving).
		tm st{};
		st.tm_year = stoi(stem.substr(0, 4)) - 1900;
		st.tm_mon = stoi(stem.substr(4, 2)) - 1;
		st.tm_mday = stoi(stem.substr(6, 2));
		st.tm_hour = stoi(stem.substr(9, 2));
		st.tm_min = stoi(stem.substr(11, 2));
		st.tm_sec = stoi(stem.substr(13, 2));
		st.tm_isdst = -1;
		tm asNamed = st;
		time_t t = mktime(&st);
		// mktime quietly rolls impossible dates over (month 13 -> January); only accept a name it didn't have to change.
		if (t != (time_t)-1 && st.tm_mon == asNamed.tm_mon && st.tm_mday == asNamed.tm_mday && st.tm_min == asNamed.tm_min && st.tm_sec == asNamed.tm_sec){
			recordingStart = double(t);
			cout << "Vehicle times: from the file name, recording started " << formatTime(recordingStart, false) << "." << endl;
			return;
		}
	}
	else {
		error_code ec;
		auto written = fs::last_write_time(source, ec);
		double frames = capture->get(CAP_PROP_FRAME_COUNT);
		if (!ec && fpsReported && frames > 0){
			// C++17 can't convert a file time to the system clock portably; both tick in real time, so shift by "now" on each.
			auto sys = chrono::system_clock::now() + chrono::duration_cast<chrono::system_clock::duration>(written - fs::file_time_type::clock::now());
			double end = chrono::duration<double>(sys.time_since_epoch()).count();
			double length = frames / g.videoFPS;
			// A modification time in the future, or a start before 2000, means the camera's clock or the copy can't be trusted.
			if (end <= clockNow() + 60 && end - length >= 946684800.0){
				recordingStart = end - length;
				cout << "Vehicle times: recording started about " << formatTime(recordingStart, false) << " (the file's modified time less its "
					<< int(length) / 60 << " min " << int(length) % 60 << " s length).  Copying a video changes its modified time;"
					<< " check this against the clock in the picture." << endl;
				return;
			}
		}
	}
	cout << "Vehicle times: unknown for this file (no valid manual_yyyymmddhhmmss name or usable modified time); "
		<< (foscamDateTime.empty() ? "Date, Time and Timestamp are" : "Timestamp is") << " left blank." << endl;
}

// ............................................ The run manifest: run_<name>.json beside the stats csv ............................................
// A csv of speeds is only as good as the record of how it was made.  Beside each stats csv VST keeps a small JSON file saying
// what was analyzed, when the footage starts and ends, the calibration and speed zone every speed rests on, the gaps in a live
// source, and how many vehicles were timed or seen but left out of the csv, so a report built on the csv can say all of that.
// The format is shared with VST Studio's report (format 1):
//   { "format": 1, "vst": "1.2.0", "stats": "<csv file name>", "source": { "input": ..., "kind": "file" | "camera" | "stream" },
//     "started": <first frame>, "ended": <end of the last frame>,   (ISO 8601 like the csv's Timestamp, or null when unknown)
//     "complete": <false while a live run is still going>, "frames": <frames analyzed>, "fps": <video fps>,
//     "outages": [ { "from": <last frame before a dropout>, "to": <frames again> } ],
//     "config": { CalibrationSpeed, CalibrationFPS, CalibrationFramesL2R/R2L, LegalDirection, L2RDirection, R2LDirection,
//                 speedLineLeft, speedLineRight, minLoggedSpeed, subFrameTiming, levelDegrees, streetEdge },
//     "counts": { "logged", "belowMinSpeed", "untimed" } }                        (see countVehicle())
// It's rewritten whole each time, to a temp file that is then renamed over it, so a reader never sees half a file: at the end
// of each video and of the run, and for a live source also when its first frames arrive, after each reconnect, and every
// 10 minutes, with "complete": false until the run ends, so a crash or power cut loses at most the last 10 minutes of it.
string manifestPath;                 // where it goes (vst::manifestPathFor() of the stats csv); empty = don't write one
string manifestStats;                // the stats csv's file name
string manifestInput, manifestKind;  // what is being analyzed (passwords removed) and file | camera | stream; see vstMain()
double runStarted = -1, runEnded = -1;  // seconds since 1970 of the run's first frame and of the end of its last; -1 = unknown
bool runHasFrames = false;           // the run's first frame pair has been read, so runStarted is settled
long long runFrames = 0;             // frames analyzed in the whole run, all videos
struct Outage { double from, to; };  // to = -1 while the source is still down
vector<Outage> outages;
chrono::steady_clock::time_point lastManifestWrite;
const int manifestEverySeconds = 600;
int runLogged = 0, runBelowMinSpeed = 0, runUntimed = 0;  // vehicle counts for the whole run; see countVehicle()

// Same folder and stem as the stats csv, with "stats_" swapped for "run_" (or "run_" put in front), and .json.
std::string vst::manifestPathFor(const std::string& statsPath){
	fs::path p(statsPath);
	string stem = p.stem().string();
	stem = stem.compare(0, 6, "stats_") == 0 ? "run_" + stem.substr(6) : "run_" + stem;
	return (p.parent_path() / (stem + ".json")).string();
}

// Called once the stats csv is open: the manifest goes beside it.
void useManifestFor(const string& statsPath){
	manifestPath = vst::manifestPathFor(statsPath);
	manifestStats = fs::path(statsPath).filename().string();
}

// s as a JSON string: quotes, backslashes and control characters escaped (RFC 8259).  Other bytes pass through, so UTF-8 stays UTF-8.
string jsonString(const string& s){
	string out = "\"";
	for (unsigned char c : s){
		switch (c){
		case '"': out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (c < 0x20){
				char u[8];
				snprintf(u, sizeof u, "\\u%04x", c);
				out += u;
			}
			else out += char(c);
		}
	}
	return out + "\"";
}

// A number as JSON: shortest form ("30", "29.97"), always with a '.' decimal point whatever the locale; null if not finite.
string jsonNumber(double v){
	if (!isfinite(v)) return "null";
	ostringstream o;
	o.imbue(locale::classic());
	o.precision(10);
	o << v;
	return o.str();
}

string jsonTime(double t){ return t >= 0 ? jsonString(formatTime(t, false)) : string("null"); }

// (Re)writes the manifest; complete = the run is over.  Failing to write it is reported but doesn't stop the run.
void writeManifest(bool complete){
	if (manifestPath.empty()) return;
	statsFile.flush();  // so the csv on disk has every row "logged" counts
	ostringstream j;
	j.imbue(locale::classic());
	j << "{\n"
		<< "  \"format\": 1,\n"
		<< "  \"vst\": " << jsonString(VST_VERSION) << ",\n"
		<< "  \"stats\": " << jsonString(manifestStats) << ",\n"
		<< "  \"source\": { \"input\": " << jsonString(manifestInput) << ", \"kind\": " << jsonString(manifestKind) << " },\n"
		<< "  \"started\": " << jsonTime(runStarted) << ",\n"
		<< "  \"ended\": " << jsonTime(runEnded) << ",\n"
		<< "  \"complete\": " << (complete ? "true" : "false") << ",\n"
		<< "  \"frames\": " << runFrames << ",\n"
		<< "  \"fps\": " << jsonNumber(g.videoFPS) << ",\n"
		<< "  \"outages\": [";
	for (size_t i = 0; i < outages.size(); i++)
		j << (i ? "," : "") << "\n    { \"from\": " << jsonTime(outages[i].from) << ", \"to\": " << jsonTime(outages[i].to) << " }";
	j << (outages.empty() ? "" : "\n  ") << "],\n"
		<< "  \"config\": {\n"
		<< "    \"CalibrationSpeed\": " << jsonNumber(g.CalibrationSpeed) << ", \"CalibrationFPS\": " << jsonNumber(g.CalibrationFPS) << ",\n"
		<< "    \"CalibrationFramesL2R\": " << g.CalibrationFramesL2R << ", \"CalibrationFramesR2L\": " << g.CalibrationFramesR2L << ",\n"
		<< "    \"LegalDirection\": " << jsonString(g.LegalDirection) << ", \"L2RDirection\": " << jsonString(g.L2RDirection)
		<< ", \"R2LDirection\": " << jsonString(g.R2LDirection) << ",\n"
		<< "    \"speedLineLeft\": " << g.speedLineLeft << ", \"speedLineRight\": " << g.speedLineRight << ",\n"
		<< "    \"minLoggedSpeed\": " << g.minLoggedSpeed << ", \"subFrameTiming\": " << g.subFrameTiming << ",\n"
		<< "    \"levelDegrees\": " << jsonNumber(g.levelDegrees) << ", \"streetEdge\": [" << g.streetEdge[0] << ", " << g.streetEdge[1] << "]\n"
		<< "  },\n"
		<< "  \"counts\": { \"logged\": " << runLogged << ", \"belowMinSpeed\": " << runBelowMinSpeed << ", \"untimed\": " << runUntimed << " }\n"
		<< "}\n";
	lastManifestWrite = chrono::steady_clock::now();  // also on failure, so a full disk is retried every 10 minutes, not every frame
	fs::path tmp = manifestPath + ".tmp";
	{
		ofstream out(tmp, ios::binary | ios::trunc);
		out << j.str();
		out.close();
		if (!out){
			cerr << "Can't write the run manifest " << tmp.string() << endl;
			return;
		}
	}
	error_code ec;
	fs::rename(tmp, manifestPath, ec);  // replaces the old manifest in one step (POSIX rename; MoveFileEx on Windows)
	if (ec){
		cerr << "Can't write the run manifest " << manifestPath << ": " << ec.message() << endl;
		fs::remove(tmp, ec);
	}
}

// The periodic rewrite of a live run's manifest, at most every manifestEverySeconds.
void manifestCheckpoint(){
	if (liveSource && chrono::steady_clock::now() - lastManifestWrite >= chrono::seconds(manifestEverySeconds)) writeManifest(false);
}

bool isVideoFile(const fs::path& p){
	string ext = p.extension().string();
	for (char& c : ext) c = (char)tolower((unsigned char)c);
	for (const char* known : { ".avi", ".mp4", ".mkv", ".mov", ".m4v", ".ts", ".h264", ".mjpeg", ".webm" })
		if (ext == known) return true;
	return false;
}

// Sorted names of the entries in dir that pass keep().
vector<string> listDir(const fs::path& dir, bool (*keep)(const fs::directory_entry&)){
	vector<string> names;
	error_code ec;
	for (const auto& e : fs::directory_iterator(dir, ec))
		if (keep(e)) names.push_back(e.path().filename().string());
	sort(names.begin(), names.end());
	return names;
}

// One line from stdin, trimmed.  Exits rather than re-prompting forever when stdin is closed (no terminal, piped input).
string ask(const string& prompt){
	cout << prompt << flush;
	string answer;
	if (!getline(cin, answer)){
		cerr << endl << "No more input on stdin.  For unattended runs use command-line mode instead (VideoSpeedTracker --help)." << endl;
		exit(EXIT_FAILURE);
	}
	size_t a = answer.find_first_not_of(" \t\r");
	size_t b = answer.find_last_not_of(" \t\r");
	return a == string::npos ? string() : answer.substr(a, b - a + 1);
}

// Asks until the answer is empty (keep dflt) or a whole number.
int askInt(const string& prompt, int dflt){
	for (;;){
		string answer = ask(prompt + " [" + to_string(dflt) + "]: ");
		if (answer.empty()) return dflt;
		try { size_t used; int v = stoi(answer, &used); if (used == answer.size()) return v; }
		catch (const exception&) {}
		cout << "Please enter a whole number." << endl;
	}
}

bool askYesNo(const string& prompt, bool dflt){
	for (;;){
		string answer = ask(prompt + (dflt ? " (y/n) [y]: " : " (y/n) [n]: "));
		if (answer.empty()) return dflt;
		if (answer == "y" || answer == "Y") return true;
		if (answer == "n" || answer == "N") return false;
	}
}

// Asks for one of names by number; "*" is also accepted when allowAll.  Returns the index, or -1 for "*".
int askChoice(const string& what, const vector<string>& names, bool allowAll){
	for (size_t i = 0; i < names.size(); i++) cout << "  " << (i + 1) << ": " << names[i] << endl;
	for (;;){
		string answer = ask("Which " + what + " (1-" + to_string(names.size()) + (allowAll ? ", or * for all" : "") + ")? ");
		if (allowAll && answer == "*") return -1;
		try { size_t used; int v = stoi(answer, &used); if (used == answer.size() && v >= 1 && v <= (int)names.size()) return v - 1; }
		catch (const exception&) {}
	}
}

// waitKey() that is a no-op when running headless, so no display is required.
int uiWaitKey(int ms){
#ifdef VST_EMBEDDED
	return hooks && hooks->pollKey ? hooks->pollKey() : -1;
#else
	return headless ? -1 : waitKey(ms);
#endif
}
//......................................................................................................................................................

//int to string helper function
string intToString(int number){
	std::stringstream ss;
	ss << number;
	return ss.str();
}

// Interact with the user via command line to gather setup information.
// Also, call readconfig() to read in (from file VST.cfg) and assign configuration data.
void setup(){

// Get configuration values from VST.cfg and set corresponding objects in Globals.h to read-in values.
	if (configPath.empty()) configPath = userpaths::resolveConfig("VST.cfg");
	if (configPath.empty() || !g.readConfig(configPath)){
		cerr << "Fix " << (configPath.empty() ? string("the config") : configPath) << " and run again." << endl;
		exit(EXIT_FAILURE);
	}
// PixelLeft is always zero relative to AnalysisBoxLeft;  PixelRight depends on AnalysisBoxWidth/
	g.pixelRight = g.AnalysisBoxWidth; // index of rightmost pixel in AnalysisBox.
	AnalysisBox = Rect(g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight);  // For use when performing speed analysis in cropped region

	// Videos live in <dataPathPrefix>/IPCam/<one folder per day>/; outputs go to <dataPathPrefix>/Stats, Trace and HiLites.
	// A blank dataPathPrefix (as shipped) means the per-user data folder, which is created with those subfolders.
	fs::path data = userpaths::dataPath(g.dataPathPrefix);
	fs::path camPath = data / "IPCam";
	cout << endl << endl;

// Get directory containing file(s) to be processed
	vector<string> dirs = listDir(camPath, [](const fs::directory_entry& e){ return e.is_directory(); });
	string dirName;
	bool allFiles = false;
	if (dirs.empty()){
		// Nothing to analyze yet, typically a first run: say where videos go, then offer the bundled sample so a bare run shows something.
		cout << "No video folders found in " << camPath.string() << endl
			<< "Put each day's videos in a folder of its own there (e.g. IPCam/20160130/), set dataPathPrefix in "
			<< configPath << " to another location, or analyze one file with --input PATH (see --help)." << endl
			<< "Setting VST up for your own camera is covered in GETTING-STARTED.md." << endl;
		fs::path sample = userpaths::sampleVideo();
		fs::path sampleConfig = userpaths::installedFile("VST.cfg");
		if (sample.empty() || sampleConfig.empty()) exit(EXIT_FAILURE);
		cout << endl;
		if (!askYesNo("Try it now on the bundled 2016 Locust Avenue sample video", true)) exit(EXIT_SUCCESS);
		// The sample only tracks with the geometry it was recorded with, which the user's config may no longer have,
		// so read the installed template over it.  Outputs still go to the data folder chosen above.
		configPath = sampleConfig.string();
		cout << endl << "Using the sample's settings from " << configPath << endl;
		if (!g.readConfig(configPath)) exit(EXIT_FAILURE);
		g.pixelRight = g.AnalysisBoxWidth;
		AnalysisBox = Rect(g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight);
		filesToDo.push_back(sample.string());
	}
	else {
		cout << "Video folders in " << camPath.string() << ":" << endl;
		dirName = dirs[askChoice("folder", dirs, false)];
		fs::path dirPath = camPath / dirName;

// Go through the file names in the selected directory for the user.
		vector<string> videos = listDir(dirPath, [](const fs::directory_entry& e){ return e.is_regular_file() && isVideoFile(e.path()); });
		if (videos.empty()){
			cerr << "No video files (.avi, .mp4, .mkv, .mov, ...) in " << dirPath.string() << endl;
			exit(EXIT_FAILURE);
		}
		cout << endl << "Videos in " << dirPath.string() << ":" << endl;
		int choice = askChoice("video", videos, true);
		allFiles = choice < 0;
		if (allFiles) for (const string& v : videos) filesToDo.push_back((dirPath / v).string());
		else filesToDo.push_back((dirPath / videos[choice]).string());
	}
	fileName = fs::path(filesToDo[0]).filename().string();

	// Do a one-time setup of region of interest, obstructions and speed posts
	Mat frame;
	openSource(capture, filesToDo[0]);
	if (!capture->isOpened() || !readFrame(*capture, frame)){
		cerr << "Can't read video from " << filesToDo[0] << endl;
		exit(EXIT_FAILURE);
	}
	cv::line(frame, Point(g.AnalysisBoxLeft, g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth, g.AnalysisBoxTop), Scalar(CVYellow), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft, g.AnalysisBoxTop + g.AnalysisBoxHeight), Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth, g.AnalysisBoxTop + g.AnalysisBoxHeight), Scalar(CVYellow), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft, g.AnalysisBoxTop), Point(g.AnalysisBoxLeft, g.AnalysisBoxTop + g.AnalysisBoxHeight), Scalar(CVYellow), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth, g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth, g.AnalysisBoxTop + g.AnalysisBoxHeight), Scalar(CVYellow), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft + g.speedLineLeft, 85 + g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.speedLineLeft, 160 + g.AnalysisBoxTop), Scalar(CVWhite), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft + g.speedLineRight, 85 + g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.speedLineRight, 160 + g.AnalysisBoxTop), Scalar(CVWhite), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft + g.obstruction[0], 85 + g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.obstruction[0], 160 + g.AnalysisBoxTop), Scalar(CVYellow), 2);
	cv::line(frame, Point(g.AnalysisBoxLeft + g.obstruction[1], 85 + g.AnalysisBoxTop), Point(g.AnalysisBoxLeft + g.obstruction[1], 160 + g.AnalysisBoxTop), Scalar(CVYellow), 2);
	for (bool l2r : { false, true }) {  // hubcap lines, at their angle if they have one
		perspective::Line l = g.lane(l2r);
		cv::line(frame, Point(g.AnalysisBoxLeft + 10, g.AnalysisBoxTop + int(lround(l.at(10)))),
			Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth - 20, g.AnalysisBoxTop + int(lround(l.at(g.AnalysisBoxWidth - 20)))), l2r ? Scalar(CVPurple) : Scalar(CVOrange), 2);
	}
	if (g.hasStreetEdge())
		cv::line(frame, Point(g.AnalysisBoxLeft, g.AnalysisBoxTop + g.streetEdge[0]), Point(g.AnalysisBoxLeft + g.AnalysisBoxWidth, g.AnalysisBoxTop + g.streetEdge[1]), Scalar(CVCyan), 1);
	double fps = capture->get(CAP_PROP_FPS);
	capture->release();

	cv::imshow("Full Frame", frame);
	waitKey(20);
	cout << endl;
	if (!askYesNo("Are Analysis Box, Speed Measuring Zone, Obstruction Framing, and Hubcap Lines OK", true)){
		cout << "Adjust the values in " << configPath << " and run again." << endl;
		cv::destroyWindow("Full Frame");
		exit(EXIT_FAILURE);
	}
	cv::destroyWindow("Full Frame");

	// Outputs are named after the folder when processing all of it, else after the one video.
	string stem = allFiles ? dirName : outputStem(fileName);
	error_code ec;
	for (const char* sub : { "Stats", "Trace", "HiLites" }) fs::create_directories(data / sub, ec);

// Want a trace file?
	cout << endl;
	pleaseTrace = askYesNo("Want a trace file", false);
	if (pleaseTrace) traceFile.open((data / "Trace" / ("trace_" + stem + ".txt")).string());

	fs::path statsPath = data / "Stats" / ("stats_" + stem + ".csv");
	statsFile.open(statsPath.string());
	if (!statsFile.is_open()){
		cerr << "Can't write " << statsPath.string() << endl;
		exit(EXIT_FAILURE);
	}
	statsFile << statsHeader << endl;
	useManifestFor(statsPath.string());

	cout << endl;
	speedLimit = askInt("Speed Limit: (int)", speedLimit);
	egregiousSpeedLowerBound = max(askInt("Egregious Speed Lower Bound: (int)", speedLimit + 10), speedLimit);
	crazySpeed = egregiousSpeedLowerBound + 20;  // Stats reporting will flag anything faster than this.

// What frame number would you like to start with in the first file?
	startFrame = askInt("Frame number to start with in first file (int)", 0);

// Want a highlights file?
	highLightsPlease = askYesNo("Want a highlights file", false);
	if (highLightsPlease){
		highLightsSpeedLower = askInt("Threshold lower speed for highlights file: (int)", highLightsSpeedLower);
		highLightsSpeedUpper = askInt("Threshold upper bound on speed for highlights file: (int)", highLightsSpeedUpper);
		minimumProfileArea = askInt("Min area of large speeding vehicle to be added to highlights (int)", minimumProfileArea);
		fs::path hiLitePath = data / "HiLites" / ("Hilites_" + stem + ".mp4");
		hiLiteVideo.open(hiLitePath.string(), VideoWriter::fourcc('m', 'p', '4', 'v'), fps > 0 ? fps : 30.0,
			Size(Globals::FrameWidth, Globals::FrameHeight), true);
		if (!hiLiteVideo.isOpened()){
			cerr << "Can't write highlights video " << hiLitePath.string() << endl;
			exit(EXIT_FAILURE);
		}
	}
	cout << "Writing " << statsPath.string() << endl;
}


// --list-pi-cameras: what `rpicam-vid --list-cameras` found, in VST's terms (the --input name for each camera, its model,
// and the frame rate it will run at with the defaults).  Exit status 0 when at least one camera is attached.
int listPiCameras(){
	bool ok = false;
	string error;
	vector<picam::Camera> cams = picam::list(ok, error);
	if (!ok){ cerr << error << endl; return EXIT_FAILURE; }
	if (cams.empty()){
		cout << "No Raspberry Pi camera found. Check the ribbon cable is pushed fully into both connectors with the contacts" << endl
			<< "facing the right way (a Pi 5 needs the 22-pin to 15-pin cable), then reboot." << endl;
		return EXIT_FAILURE;
	}
	picam::Settings defaults;
	for (const picam::Camera& c : cams)
		cout << "picam:" << c.index << "    " << c.model << " (" << c.sensor << "), " << picam::chooseFps(defaults, &c) << " fps at "
			<< defaults.width << "x" << defaults.height << (c.autofocus ? ", focus locked at 0.25 dioptres unless focus= says otherwise" : "") << endl;
	return EXIT_SUCCESS;
}

void printUsage(){
	cout << "Usage: VideoSpeedTracker [options]        (no options = original interactive mode)" << endl
		<< "  --demo                analyze the bundled 2016 sample video with the settings it was recorded with" << endl
		<< "  --input PATH          video file to analyze (required in command-line mode, unless --demo)" << endl
		<< "  --config PATH         VST.cfg to use [./VST.cfg, else the per-user config, created on first run]" << endl
		<< "  --stats PATH          csv output [stats_<input name>.csv]" << endl
		<< "  --trace PATH          write a debug trace file" << endl
		<< "  --speed-limit N       [25]" << endl
		<< "  --egregious N         egregious speed lower bound [speed limit + 10]" << endl
		<< "  --start-frame N       [0]" << endl
		<< "  --highlights PATH     write a highlights video (mp4v) of qualifying vehicles" << endl
		<< "  --hl-min N --hl-max N speed range for highlights [35..100]" << endl
		<< "  --hl-area N           minimum profile area for large-vehicle highlights [100]" << endl
		<< "  --headless            no windows; runs as fast as the CPU allows" << endl
		<< "  --list-pi-cameras     list the Raspberry Pi cameras attached, then exit" << endl
		<< "Inputs: a video file, a stream URL (rtsp://, http://), a USB camera number (0, 1, ...), or a Raspberry Pi camera:" << endl
		<< "  picam, picam:1, or picam:0?fps=60&shutter=1000&gain=0&focus=0.25&size=1280x720&rotate=180 (quote it in a shell)." << endl
		<< "  Defaults: 60 fps, 1/1000 s shutter, automatic gain, Camera Module 3 focus locked at 0.25 dioptres (4 m)." << endl
		<< "Video of any size or frame rate is accepted: frames are scaled to 1280x720 (the size VST.cfg coordinates" << endl
		<< "refer to) and speeds are scaled by CalibrationFPS.  Exit status is 0 on success, 1 on any error." << endl
		<< "First time?  Try --demo, then see GETTING-STARTED.md to set VST up for your own camera." << endl;
}

// Command-line counterpart of setup(): same globals end up set, but from arguments instead of prompts,
// and with portable paths so it runs on Linux and macOS as well as Windows.
bool setupFromArgs(int argc, char** argv){
	string statsPath, tracePath, hiLitePath;
	bool egregiousGiven = false;
	for (int i = 1; i < argc; i++){
		string a = argv[i];
		// Both "--opt value" and "--opt=value" are accepted.
		string inlineValue;
		bool hasInline = false;
		size_t eq = a.find('=');
		if (a.compare(0, 2, "--") == 0 && eq != string::npos){ inlineValue = a.substr(eq + 1); a = a.substr(0, eq); hasInline = true; }
		auto next = [&](){
			if (hasInline) return inlineValue;
			if (i + 1 >= argc){ cerr << "Missing value for " << a << endl; quit(EXIT_FAILURE); }
			return string(argv[++i]);
		};
		auto nextInt = [&](){
			string v = next();
			try { size_t used; int n = stoi(v, &used); if (used == v.size()) return n; }
			catch (const exception&) {}
			cerr << a << " needs a whole number, got \"" << v << "\"" << endl;
			quit(EXIT_FAILURE);
		};
		if (a == "--input") inputPath = next();
		else if (a == "--config") configPath = next();
		else if (a == "--stats") statsPath = next();
		else if (a == "--trace") tracePath = next();
		else if (a == "--speed-limit") speedLimit = nextInt();
		else if (a == "--egregious"){ egregiousSpeedLowerBound = nextInt(); egregiousGiven = true; }
		else if (a == "--start-frame") startFrame = nextInt();
		else if (a == "--highlights") hiLitePath = next();
		else if (a == "--hl-min") highLightsSpeedLower = nextInt();
		else if (a == "--hl-max") highLightsSpeedUpper = nextInt();
		else if (a == "--hl-area") minimumProfileArea = nextInt();
		else if (a == "--headless") headless = true;
		else if (a == "--demo") demo = true;
		else if (a == "--list-pi-cameras"){ quit(listPiCameras()); }
		else if (a == "--help" || a == "-h"){ printUsage(); quit(EXIT_SUCCESS); }
		else { cerr << "Unknown option: " << a << endl; printUsage(); return false; }
	}
	if (demo){
		// The sample plus the template it was calibrated with (unless --config overrides it); the csv goes to the
		// per-user Stats folder rather than the working directory, which may be the read-only install folder.
		fs::path sample = userpaths::sampleVideo();
		if (sample.empty()){
			cerr << "The sample video isn't installed next to this program (looked for sample/manual_20160130172251.avi)." << endl
				<< "Get it from https://github.com/joekiller/VideoSpeedTracker/tree/main/sampleData/IPCam/20160130 and pass it with --input." << endl;
			return false;
		}
		if (!inputPath.empty()){ cerr << "--demo and --input can't be used together" << endl; return false; }
		inputPath = sample.string();
		if (configPath.empty()){
			fs::path tmpl = userpaths::installedFile("VST.cfg");
			if (!tmpl.empty()) configPath = tmpl.string();
		}
		if (statsPath.empty()){
			fs::path stats = userpaths::dataDir() / "Stats";
			error_code ec;
			fs::create_directories(stats, ec);
			statsPath = (stats / "stats_demo.csv").string();
		}
	}
	if (inputPath.empty()){ cerr << "--input is required" << endl; printUsage(); return false; }

	if (configPath.empty()) configPath = userpaths::resolveConfig("VST.cfg");
	if (configPath.empty() || !g.readConfig(configPath)){
		cerr << "Fix " << (configPath.empty() ? string("the config") : configPath) << " and run again." << endl;
		return false;
	}
	g.pixelRight = g.AnalysisBoxWidth;
	AnalysisBox = Rect(g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight);

	filesToDo = { inputPath };
	fileName = fs::path(inputPath).filename().string();  // Foscam names fill the csv Date/Time columns

	if (!egregiousGiven) egregiousSpeedLowerBound = speedLimit + 10;
	egregiousSpeedLowerBound = max(egregiousSpeedLowerBound, speedLimit);
	crazySpeed = egregiousSpeedLowerBound + 20;

	if (statsPath.empty()){
		picam::Settings pi;
		string piError;
		string stem = isPiCamera(inputPath) && picam::parse(inputPath, pi, piError) ? "picam" + to_string(pi.camera) : fs::path(fileName).stem().string();
		statsPath = "stats_" + stem + ".csv";
	}
	statsFile.open(statsPath);
	if (!statsFile.is_open()){ cerr << "Can't write stats file " << statsPath << endl; return false; }
	statsFile << statsHeader << endl;
	useManifestFor(statsPath);

	pleaseTrace = !tracePath.empty();
	if (pleaseTrace){
		traceFile.open(tracePath);
		if (!traceFile.is_open()){ cerr << "Can't write trace file " << tracePath << endl; return false; }
	}

	highLightsPlease = !hiLitePath.empty();
	if (highLightsPlease){
		// A Pi camera isn't started just to ask its rate (that takes a couple of seconds, and only one reader may
		// have it): the rate it will be asked for is known from its name.
		double fps = 30.0;
		picam::Settings pi;
		string piError;
		if (isPiCamera(inputPath)){ if (picam::parse(inputPath, pi, piError)) fps = picam::chooseFps(pi, nullptr); }
		else {
			unique_ptr<frameio::FrameSource> probe = frameio::closed();
			openSource(probe, inputPath);
			if (probe->isOpened()) fps = probe->get(CAP_PROP_FPS);
		}
		hiLiteVideo.open(hiLitePath, VideoWriter::fourcc('m', 'p', '4', 'v'), fps > 0 ? fps : 30.0, Size(Globals::FrameWidth, Globals::FrameHeight), true);
		if (!hiLiteVideo.isOpened()){ cerr << "Can't write highlights video " << hiLitePath << endl; return false; }
	}
	cout << "Analyzing " << withoutPassword(inputPath) << " -> " << statsPath << (headless ? "  (headless)" : "") << endl;
	return true;
}


string vStateString(vehicleStatus inState){
	// {entering, inMiddle, exiting, exited};
	if (inState == entering) return "entering";
	else if (inState == inMiddle) return "inMiddle";
	else if (inState == exiting) return "exiting";
	else return "exited";
}


string statusString(statusTypes inStatus){
	// statusTypes {ImOK, deleteWithStats, lostTrack, negVelocity }
	if (inStatus == ImOK) return "ImOK";
	else if (inStatus == deleteWithStats) return "deleteWithStats";
	else if (inStatus == lostTrack) return "lostTrack";
	else return "negVelocity";

}


string overlapString(OverlapType inOverlap){
	//	none, rearOnly, frontOnly, bothOverlap
	if (inOverlap == none) return "none";
	else if (inOverlap == rearOnly) return "rearOnly";
	else if (inOverlap == frontOnly) return "frontOnly";
	else return "bothOverlap";
}



void hesitate(int code){  // easy breakpoint for debugging when you don't want to fire up a debugger.
	cout << frameNumber << "  Program paused, input value is: " << code << "   Press 'p' to resume" << endl;
	while (waitKey() != 112);
}



Rect coalesce(Rect rectangles[], int numRects, int loX, int hiX, grabType how){
	// For a specified region of interest, put a single rectangle around all of the external contours the contours funtion found.
	Rect retRect;
	int firstOK = -1;
	for (int i = 0; i < numRects; i++){ // find first if any rectangle that is in spec'd bounds
		if (rectangles[i].x <= hiX && (rectangles[i].x + rectangles[i].width) >= loX){ // Any overlap at all?
			retRect = rectangles[i];
			if (how == strict) {  // Keep it inside LoX...HiX
				retRect.x = max(rectangles[i].x, loX);
				retRect.width = min(rectangles[i].x + rectangles[i].width, (loX + hiX)) - retRect.x;
			}
			firstOK = i;
			break;
		}
	}
	if (firstOK == -1){
		if(pleaseTrace) traceFile << "<" << frameNumber << "> Coalesce finds no acceptable objects between loX:  " << loX << " and  hiX:  " << hiX << endl;
		return Rect{ -1, 0, 0, 0 };
	}
	for (int i = (firstOK + 1); i < numRects; i++){
		if (rectangles[i].x <= hiX && (rectangles[i].x + rectangles[i].width) >= loX){    // overlap with requested bounded area
			int leftMore = min(retRect.x, rectangles[i].x);
			int rightMore = max(retRect.x + retRect.width, rectangles[i].x + rectangles[i].width);
			if (how == strict) {  // Keep it inside LoX...HiX
				leftMore = max(leftMore, loX);
				rightMore = min(rightMore, (loX + hiX));
			}
			retRect.x = leftMore;
			retRect.width = rightMore - retRect.x;

			int topMore = min(retRect.y, rectangles[i].y);
			int bottomMore = max(retRect.y + retRect.height, rectangles[i].y + rectangles[i].height);
			retRect.y = topMore;
			retRect.height = bottomMore - retRect.y;
		}
	}
	if (pleaseTrace) traceFile << "<" << frameNumber << "> Coalesce finds object in [" << loX << ", " << hiX << "] --> Rect:   " << retRect.x << ", "
		<< retRect.y << ", " << retRect.width << ",   " << retRect.height;// << endl;
	return retRect;

}

bool meetsHLRCriterion(int inSpeed, int inArea){ // Does the vehicle speed meet criterion for HiLites reel?
	return highLightsPlease 
		&& (   ((inSpeed >= highLightsSpeedLower) && (inSpeed <= highLightsSpeedUpper))
		||    /* ((inSpeed >= (highLightsSpeedLower - 8)) && */ (inArea >= g.largeVehicleArea) /*)*/);
}

void displayAnalysisGoingRight(int inFrameNum, int index, Rect rectangle, OverlapType Olap, Mat &AnalysisFrame, int estSpeed){
// Display the green rectangle with the leading blue vertical line (hopefully on the front bumper) representng the *predicted*
// area occupied by the vehicle.  Also display the velocity of the vehicle after it has has passed its second white post delineating the end
// of the L2R speed measuring zone.  Also, save frame until it's known whether this vehicle will be added to highlights video.
	int x = rectangle.x;
	int y = rectangle.y;
	int wd = rectangle.width;
	int ht = rectangle.height;
	cv::line(AnalysisFrame, Point(x, y), Point(x + wd, y), Scalar(CVGreen), 2);
	cv::line(AnalysisFrame, Point(x, y + ht), Point(x + wd, y + ht), Scalar(CVGreen), 2);
	if (Olap == rearOnly || Olap == bothOverlap)
		cv::line(AnalysisFrame, Point(x, y), Point(x, y + ht), Scalar(CVRed), 2);
	else
		cv::line(AnalysisFrame, Point(x, y), Point(x, y + ht), Scalar(CVGreen), 2);
	if (Olap == frontOnly || Olap == bothOverlap)
		cv::line(AnalysisFrame, Point(x + wd, y), Point(x + wd, y + ht), Scalar(CVRed), 2);
	else
		cv::line(AnalysisFrame, Point(x + wd, y), Point(x + wd, y + ht), Scalar(CVBlue), 2);
	if (estSpeed > 0) {
		if (estSpeed <= speedLimit)  
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelRight-180, 30), 2, 1, Scalar(CVGreen), 2);
		else if (estSpeed < egregiousSpeedLowerBound) 
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelRight-180, 30), 2, 1, Scalar(CVYellow), 2);
		else                         
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelRight-180, 30), 2, 1, Scalar(CVRed), 2);
	}
	if (highLightsPlease){
		if (vehiclesGoingRight[index].getTrackStartPixel() == 0)
			vehiclesGoingRight[index].holdFrame(AnalysisFrame); // if vehicle hasnt passed start post yet, keep last frame in case about to cross into speed zone.
		else {  // past the start post,
			if (vehiclesGoingRight[index].getNumberSavedFrames() == 0) // First time to save a frame for hilites reel?
				vehiclesGoingRight[index].saveFrame(vehiclesGoingRight[index].getHeldFrame()); // then start up saving frames to possibly copy to hilites file later on.
			if (estSpeed <= 0) vehiclesGoingRight[index].saveFrame(AnalysisFrame); // Be saving frames past the start post.
			else  // estSpeed is > 0 meaning vehicle has passed end post
				if (meetsHLRCriterion(estSpeed, vehiclesGoingRight[index].getArea()) && vehiclesGoingRight[index].getTrackEndFrame() == frameNumber) // No use saving frames if vehicle doesn't meet hilites Reel criterion
					vehiclesGoingRight[index].saveFrame(AnalysisFrame);
		}
	}
#ifdef VST_EMBEDDED
	frameBoxes.push_back({ true, rectangle, max(estSpeed, 0) });
#endif
	if (pleaseTrace) traceFile << "<" << frameNumber << "> DisplayGoingRight... Rect:  " << x << ", " << y << ", " << wd << ",  " << ht
		<<endl << endl;
}


void displayAnalysisGoingLeft(int inFrameNum, int index, Rect rectangle, OverlapType Olap, Mat &AnalysisFrame, int estSpeed){
// Display the green rectangle with the leading blue vertical line (hopefully on the front bumper) representng the *predicted*
// area occupied by the vehicle.  Also display the velocity of the vehicle after it has has passed its second white post, delineating the end
// of the R2L speed measuring zone.  Also, save frame until it's known whether this vehicle will be added to highlights video.
	int x = rectangle.x;
	int y = rectangle.y;
	int wd = rectangle.width;
	int ht = rectangle.height;
	cv::line(AnalysisFrame, Point(x, y), Point(x + wd, y), Scalar(CVGreen), 2);
	cv::line(AnalysisFrame, Point(x, y + ht), Point(x + wd, y + ht), Scalar(CVGreen), 2);
	if (Olap == frontOnly || Olap == bothOverlap)
		cv::line(AnalysisFrame, Point(x, y), Point(x, y + ht), Scalar(CVRed), 2);
	else
		cv::line(AnalysisFrame, Point(x, y), Point(x, y + ht), Scalar(CVBlue), 2);
	if (Olap == rearOnly || Olap == bothOverlap)
		cv::line(AnalysisFrame, Point(x + wd, y), Point(x + wd, y + ht), Scalar(CVRed), 2);
	else
		cv::line(AnalysisFrame, Point(x + wd, y), Point(x + wd, y + ht), Scalar(CVGreen), 2);
	if (estSpeed > 0) {
		if(estSpeed <= speedLimit) 
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelLeft, 30), 2, 1, Scalar(CVGreen), 2);
		else if (estSpeed < egregiousSpeedLowerBound)
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelLeft, 30), 2, 1, Scalar(CVYellow), 2);
		else 
			putText(AnalysisFrame, intToString(estSpeed) + " MPH", Point(g.pixelLeft, 30), 2, 1, Scalar(CVRed), 2);
	}
	if (highLightsPlease){
		if (vehiclesGoingLeft[index].getTrackStartPixel() == 0) 
			vehiclesGoingLeft[index].holdFrame(AnalysisFrame); // if vehicle hasnt passed start post yet, keep last frame in case about to cross into speed zone.
		else {  // past the start post,
			if (vehiclesGoingLeft[index].getNumberSavedFrames() == 0) // First time to save a frame for hilites reel?
				vehiclesGoingLeft[index].saveFrame(vehiclesGoingLeft[index].getHeldFrame()); // then start up saving frames to possibly copy to hilites file later on.
			if (estSpeed <= 0) vehiclesGoingLeft[index].saveFrame(AnalysisFrame); // Be saving frames past the start post.
			else  // estSpeed is > 0 meaning vehicle has passed end post
				if (meetsHLRCriterion(estSpeed, vehiclesGoingLeft[index].getArea()) && vehiclesGoingLeft[index].getTrackEndFrame() == frameNumber) // No use saving frames after end post if vehicle doesn't meet hilites Reel criterion
					vehiclesGoingLeft[index].saveFrame(AnalysisFrame);
		}
	}
#ifdef VST_EMBEDDED
	frameBoxes.push_back({ false, rectangle, max(estSpeed, 0) });
#endif
	if (pleaseTrace) traceFile << "<" << frameNumber << "> DisplayGoingLeft... Rect:  " << x << ", " << y << ", " << wd << ",  " << ht
		<< endl << endl;
}







// Per-run totals for the summary printed at the end of each video.
int loggedVehicles = 0, speeders = 0, wrongWayVehicles = 0;

// Decides whether a vehicle the tracker is letting go of gets a csv row, and counts it for the run manifest either way.
// Every vehicle comes through here once: from logL2Rstats()/logR2Lstats() when it exits the analysis box (isOK true) or
// when manageMovers() deletes it (isOK true only if it had already been timed), or from dropTracks() when the tracker
// gives up on everything it is following.
//   timed          VehicleDynamics measured a speed: the front bumper crossed the second speed line with entry and exit gaps
//                  that agree (validGap()).  That is the only place trackEndPixel becomes > 0, and finalSpeed is set with it.
//   logged         timed, isOK, and estSpeed >= minLoggedSpeed: a csv row.
//   belowMinSpeed  timed and isOK, but slower than minLoggedSpeed (walkers, cyclists, a car turning in): counted, no row.
//   untimed        tracked from outside the speed zone across its first line (trackStartPixel set), but never got a speed
//                  that could be logged: deleted in the zone (lost track, hidden by an oncoming vehicle, negative velocity),
//                  an invalid measurement (validGap() failed), or dropped when the tracker bailed, the source dropped out or
//                  the video ended before it left the box.
// Not counted at all: tracks that never reached the first speed line (mostly noise at the ends of the box, and none of them
// could have been timed), and tracks first seen already inside the zone (markInvalidSpeed()).  Those are nearly always a
// lost vehicle being picked up again, often several times over a few frames (26 of them on the 2016 sample, against 5
// untimed vehicles), so counting them would count one car many times.
// So logged + belowMinSpeed is the number of timed vehicles, the same whatever minLoggedSpeed is, and minLoggedSpeed = 0
// logs every one of them.  With the default 18 this is exactly the pre-v1.2 rule (estSpeed >= 18 and isOK), since a
// vehicle's estSpeed is -1 until it is timed.
// An embedding host also hears about each belowMinSpeed and untimed vehicle (Hooks::onAttempt), so it can show how many cars
// were tried against how many got a speed; a logged one reaches it through reportVehicle() instead.
void reportAttempt(bool leftToRight, bool timed, int estSpeed){
#ifdef VST_EMBEDDED
	if (hooks && hooks->onAttempt) hooks->onAttempt({ leftToRight, timed, timed ? estSpeed : 0, frameNumber });
#else
	(void)leftToRight; (void)timed; (void)estSpeed;
#endif
}

bool countVehicle(bool isOK, VehicleDynamics& v, int estSpeed, bool leftToRight){
	if (isOK && v.getTrackEndPixel() > 0){
		if (estSpeed >= g.minLoggedSpeed){ runLogged++; return true; }
		runBelowMinSpeed++;
		reportAttempt(leftToRight, true, estSpeed);
	}
	else if (v.getTrackStartPixel() != 0 && !v.firstSeenInZone()){
		runUntimed++;
		reportAttempt(leftToRight, false, -1);
		if (pleaseTrace) traceFile << "<" << frameNumber << ">   UNTIMED " << (leftToRight ? "L2R" : "R2L") << " vehicle: entered the zone at frame "
			<< v.getTrackStartFrame() << ", pixel " << v.getTrackStartPixel() << ", and was lost before timing finished" << endl;
	}
	return false;
}

// Forgets every vehicle being tracked, counting each one that was in the speed zone as untimed (see countVehicle()).
void dropTracks(){
	for (auto& v : vehiclesGoingRight) countVehicle(false, v, -1, true);
	for (auto& v : vehiclesGoingLeft) countVehicle(false, v, -1, false);
	vehiclesGoingRight.clear();
	vehiclesGoingLeft.clear();
}

// Counts a vehicle that was just written to the stats csv, calls out wrong-way travel on a one-way street
// (LegalDirection in VST.cfg), and hands the vehicle to an embedding host.
void reportVehicle(bool leftToRight, VehicleDynamics& v, int estSpeed, const string& timestamp){
	bool wrongWay = g.isWrongWay(leftToRight);
	const string& compass = leftToRight ? g.L2RDirection : g.R2LDirection;
	loggedVehicles++;
	if (estSpeed > speedLimit) speeders++;
	if (wrongWay){
		wrongWayVehicles++;
		cout << "<" << frameNumber << ">  WRONG WAY: " << (leftToRight ? "L2R" : "R2L") << " (" << compass << ") at " << estSpeed << " MPH" << endl;
	}
#ifdef VST_EMBEDDED
	if (hooks && hooks->onVehicle)
		hooks->onVehicle({ leftToRight, wrongWay, compass, estSpeed, frameNumber, v.getTrackStartFrame(), v.getTrackEndFrame(),
			v.getTrackStartPixel(), v.getTrackEndPixel(), v.getArea(), timestamp, v.getSpeedTrend() });
#else
	(void)v;
	(void)timestamp;
#endif
}

// The trace file's line for a speed trend: "accelerating  in 22 out 28 mph  +5.3 +- 1.1 mph/s  31 mph in 20 ft  (11 points)".
string traceTrend(const vst::SpeedTrend& t){
	char buf[160];
	if (!t.known()) snprintf(buf, sizeof buf, "unknown  (%d points, %.2f ft rms, %+.1f +- %.1f mph/s)", t.points, t.rmsFt, t.accelMphPerSec, t.accelSE);
	else snprintf(buf, sizeof buf, "%s  in %.1f out %.1f mph  %+.1f +- %.1f mph/s  %.1f mph in 20 ft  (%d points, %.2f ft rms)", vst::trendName(t.trend),
		t.entryMph, t.exitMph, t.accelMphPerSec, t.accelSE, t.projectedMph, t.points, t.rmsFt);
	return buf;
}

void logL2Rstats(bool isOK, int index){
// Final entries for L2R vehicle just completing speed analysis are placed in trace file and in stats files.  Video output to highlights
//	file for qualifying vehicles is performed.
	int frames = max(vehiclesGoingRight[index].getTrackEndFrame() - vehiclesGoingRight[index].getTrackStartFrame(), 1);
	int estSpeed = vehiclesGoingRight[index].getFinalSpeed();
	if (pleaseTrace) traceFile << "<" << frameNumber << ">   Entry frame: " << vehiclesGoingRight[index].getTrackStartFrame()
		<< "   Exit frame : " << vehiclesGoingRight[index].getTrackEndFrame()
		<< "   # frames: " << (vehiclesGoingRight[index].getTrackEndFrame() - vehiclesGoingRight[index].getTrackStartFrame())
		<< endl
		<< "         Entry pixel: " << vehiclesGoingRight[index].getTrackStartPixel()
		<< "   Exit pixel: " << vehiclesGoingRight[index].getTrackEndPixel()
		<< "   # Pixels: " << vehiclesGoingRight[index].getTrackEndPixel() - vehiclesGoingRight[index].getTrackStartPixel()
		<< "             Est speed: " << estSpeed
		<< endl << "         Trend: " << traceTrend(vehiclesGoingRight[index].getSpeedTrend())
		<< endl << "> > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > >"
		<< endl << endl << endl;
	if (countVehicle(isOK, vehiclesGoingRight[index], estSpeed, true)){  // timed, clean track, and at least minLoggedSpeed
		double when = vehicleTime();
		string timestamp = when >= 0 ? formatTime(when, false) : "";
		statsFile << csvDateTime(when) << ", "
			<< frameNumber << ", " << g.L2RDirection << ", " << vehiclesGoingRight[index].getTrackStartFrame() << ", "
			<< vehiclesGoingRight[index].getTrackEndFrame() << ", "
			<< frames << ", "
			<< vehiclesGoingRight[index].getTrackStartPixel() << ", "
			<< vehiclesGoingRight[index].getTrackEndPixel() << ", "
			<< vehiclesGoingRight[index].getTrackEndPixel() - vehiclesGoingRight[index].getTrackStartPixel() << ", "
			<< vehiclesGoingRight[index].getArea() << ", , "
			<< estSpeed << ", "
			<< ((estSpeed < 0 || estSpeed > crazySpeed) ? "*****" : "") << ", "
			<< timestamp << ", " << trendCells(vehiclesGoingRight[index].getSpeedTrend()) << endl;
		reportVehicle(true, vehiclesGoingRight[index], estSpeed, timestamp);
		if (meetsHLRCriterion(estSpeed, vehiclesGoingRight[index].getArea())){
			Mat zero = Mat::zeros(Size(Globals::FrameWidth, Globals::FrameHeight), frame1.type());
			int speedleft = AnalysisBox.x + g.speedLineLeft; // Left boundary may have moved right for ROI boundary.
			int speedRight = AnalysisBox.x + g.speedLineRight;
			int arrowY = AnalysisBox.y - 32;
			frame1(Rect(0, 0, 240, 29)).copyTo(zero(Rect(500, 440, 240, 29))); // Get date/time from input frame and copy it to just below the ROI.
			arrowedLine(zero, Point(speedleft + 10, arrowY), Point(speedleft + 60, arrowY), Scalar(CVPurple), 5);
			for (int i = 0; i < 5; i++){
				vehiclesGoingRight[index].getSavedFrame(0).copyTo(zero(AnalysisBox));
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			arrowedLine(zero, Point(speedleft + 10, arrowY), Point(speedleft + 60, arrowY), Scalar(CVBlack), 5);
			int midPoint = (speedleft + speedRight) / 2;
			arrowedLine(zero, Point(midPoint - 25, arrowY), Point(midPoint + 25, arrowY), Scalar(CVPurple), 5);
			for (int i = 0; i <= vehiclesGoingRight[index].getNumberSavedFrames() - 1; i++){
				vehiclesGoingRight[index].getSavedFrame(i).copyTo(zero(AnalysisBox));
				if (i == vehiclesGoingRight[index].getNumberSavedFrames() - 1){
					arrowedLine(zero, Point(midPoint - 25, arrowY), Point(midPoint + 25, arrowY), Scalar(CVBlack), 5);
					arrowedLine(zero, Point(speedRight - 60, arrowY), Point(speedRight - 10, arrowY), Scalar(CVPurple), 5);
					if (estSpeed >= egregiousSpeedLowerBound) 
						putText(zero, intToString(estSpeed) + " MPH", Point(speedRight - 125, AnalysisBox.y - 55), 2, 1, Scalar(CVRed), 2);
					else if (estSpeed > speedLimit)
						putText(zero, intToString(estSpeed) + " MPH", Point(speedRight - 125, AnalysisBox.y - 55), 2, 1, Scalar(CVYellow), 2);
					else putText(zero, intToString(estSpeed) + " MPH", Point(speedRight - 125, AnalysisBox.y - 55), 2, 1, Scalar(CVGreen), 2);
				}
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			for (int i = 0; i < 5; i++){
				vehiclesGoingRight[index].getSavedFrame(vehiclesGoingRight[index].getNumberSavedFrames() - 1).copyTo(zero(AnalysisBox));
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			hiLiteVideo.write(Mat::zeros(Size(Globals::FrameWidth, Globals::FrameHeight), frame1.type())); //  write a partition between vehicles for HiLites processor to detect.
		}
	}
}

void logR2Lstats(bool isOK, int index){
// Final entries for R2L vehicle just completing speed analysis are placed in trace file and in stats files.  Video output to highlights
//	file for qualifying vehicles is performed.
	int frames = max(vehiclesGoingLeft[index].getTrackEndFrame() - vehiclesGoingLeft[index].getTrackStartFrame(), 1);
	int estSpeed = vehiclesGoingLeft[index].getFinalSpeed();
	if (pleaseTrace) traceFile
		<< "<" << frameNumber << ">   Start frame: " << vehiclesGoingLeft[index].getTrackStartFrame()
		<< "   End frame : " << vehiclesGoingLeft[index].getTrackEndFrame()
		<< "   # frames: " << (vehiclesGoingLeft[index].getTrackEndFrame() - vehiclesGoingLeft[index].getTrackStartFrame())
		<< endl
		<< "          Start pixel: " << vehiclesGoingLeft[index].getTrackStartPixel()
		<< "   End pixel: " << vehiclesGoingLeft[index].getTrackEndPixel()
		<< "   # Pixels: " << vehiclesGoingLeft[index].getTrackStartPixel() - vehiclesGoingLeft[index].getTrackEndPixel()
		<< "             Est speed: " << estSpeed
		<< endl << "         Trend: " << traceTrend(vehiclesGoingLeft[index].getSpeedTrend())
		<< endl << "< < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < < <"
		<< endl << endl << endl;
	if (countVehicle(isOK, vehiclesGoingLeft[index], estSpeed, false)){  // timed, clean track, and at least minLoggedSpeed
		double when = vehicleTime();
		string timestamp = when >= 0 ? formatTime(when, false) : "";
		statsFile << csvDateTime(when) << ", "
			<< frameNumber << ", " << g.R2LDirection << ", " << vehiclesGoingLeft[index].getTrackStartFrame() << ", "
			<< vehiclesGoingLeft[index].getTrackEndFrame() << ", "
			<< frames << ", "
			<< vehiclesGoingLeft[index].getTrackStartPixel() << ", "
			<< vehiclesGoingLeft[index].getTrackEndPixel() << ", "
			<< vehiclesGoingLeft[index].getTrackStartPixel() - vehiclesGoingLeft[index].getTrackEndPixel() << ", "
			<< vehiclesGoingLeft[index].getArea() << ", , "
			<< estSpeed << ", "
			<< ((estSpeed < 0 || estSpeed > crazySpeed) ? "*****" : "") << ", "
			<< timestamp << ", " << trendCells(vehiclesGoingLeft[index].getSpeedTrend()) << endl;
		reportVehicle(false, vehiclesGoingLeft[index], estSpeed, timestamp);
		if (meetsHLRCriterion(estSpeed, vehiclesGoingLeft[index].getArea())){
			Mat zero = Mat::zeros(Size(Globals::FrameWidth, Globals::FrameHeight), frame1.type());
			int speedleft = AnalysisBox.x + g.speedLineLeft;
			int speedRight = AnalysisBox.x + g.speedLineRight;
			int arrowY = AnalysisBox.y - 32;
			frame1(Rect(0, 0, 240, 29)).copyTo(zero(Rect(500, 440, 240, 29))); // Get date/time from input frame and copy it to just below the ROI.
			arrowedLine(zero, Point(speedRight - 10, arrowY), Point(speedRight - 60, arrowY), Scalar(CVOrange), 5);
			for (int i = 0; i < 5; i++){
				vehiclesGoingLeft[index].getSavedFrame(0).copyTo(zero(AnalysisBox));
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			arrowedLine(zero, Point(speedRight - 10, arrowY), Point(speedRight - 60, arrowY), Scalar(CVBlack), 5);
			int midPoint = (speedleft + speedRight) / 2;
			arrowedLine(zero, Point(midPoint + 25, arrowY), Point(midPoint - 25, arrowY), Scalar(CVOrange), 5);
			for (int i = 0; i <= vehiclesGoingLeft[index].getNumberSavedFrames() - 1; i++){
				vehiclesGoingLeft[index].getSavedFrame(i).copyTo(zero(AnalysisBox));
				if (i == vehiclesGoingLeft[index].getNumberSavedFrames() - 1){
					arrowedLine(zero, Point(midPoint + 25, arrowY), Point(midPoint - 25, arrowY), Scalar(CVBlack), 5);
					arrowedLine(zero, Point(speedleft + 60, arrowY), Point(speedleft + 10, arrowY), Scalar(CVOrange), 5);
					if (estSpeed >= egregiousSpeedLowerBound)
						putText(zero, intToString(estSpeed) + " MPH", Point(speedleft, AnalysisBox.y - 55), 2, 1, Scalar(CVRed), 2);
					else if (estSpeed > speedLimit)
						putText(zero, intToString(estSpeed) + " MPH", Point(speedleft, AnalysisBox.y - 55), 2, 1, Scalar(CVYellow), 2);
					else putText(zero, intToString(estSpeed) + " MPH", Point(speedleft, AnalysisBox.y - 55), 2, 1, Scalar(CVGreen), 2);
				}
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			for (int i = 0; i < 5; i++){
				vehiclesGoingLeft[index].getSavedFrame(vehiclesGoingLeft[index].getNumberSavedFrames() - 1).copyTo(zero(AnalysisBox));
				hiLiteVideo.write(zero);
				uiWaitKey(10);
			}
			hiLiteVideo.write(Mat::zeros(Size(Globals::FrameWidth, Globals::FrameHeight), frame1.type())); //  write a partition between vehicles for HiLites processor to detect.
		}
	}
}


OverlapType doesL2ROverlapAnyR2L(int L2RIndex, vector<Projection> vehiclesL2R, vector<Projection> vehiclesR2L, int projectedR2LSize){
// Function name tells it all.  Does a selected L2R vehicle overlap any R2L vehicles?  Indicate which bumpers overlap.
	bool frontOverlap = false;
	bool rearOverlap = false;
	int L2RRearBumper = max(vehiclesL2R[L2RIndex].getBox().x - (4 * g.SLOP), g.pixelLeft);
	int L2RFrontBumper = min(L2RRearBumper + vehiclesL2R[L2RIndex].getBox().width + (5 * g.SLOP), g.pixelRight);  // 5x makes up for SLOP subtracted from rear bumper.
	for (int R2LIndex = 0; R2LIndex < projectedR2LSize; R2LIndex++){
		int R2LFrontBumper = vehiclesR2L[R2LIndex].getBox().x;
		int R2LRearBumper = R2LFrontBumper + vehiclesR2L[R2LIndex].getBox().width;
		if ((L2RFrontBumper >= R2LFrontBumper) && (L2RFrontBumper <= R2LRearBumper)) frontOverlap = true;
		if ( ( (L2RRearBumper >= R2LFrontBumper) && (L2RRearBumper <= R2LRearBumper) )
		/*	|| ((L2RRearBumper <= R2LFrontBumper) && (L2RFrontBumper >= R2LRearBumper)) */ ) rearOverlap = true;  // Handle full obstruction case
	}
	// none, rearOnly, frontOnly, bothOverlap
	if (frontOverlap && rearOverlap) return bothOverlap;
	if (frontOverlap) return frontOnly;
	if (rearOverlap) return rearOnly;
	return none;
}


OverlapType doesR2LOverlapAnyL2R(int R2LIndex, vector<Projection> vehiclesR2L, vector<Projection> vehiclesL2R, int projectedL2RSize){
// Function name tells it all.  Does a selected R2L vehicle overlap any L2R vehicles?  Indicate which bumpers overlap.
	bool frontOverlap = false;
	bool rearOverlap = false;
	int R2LFrontBumper = max(vehiclesR2L[R2LIndex].getBox().x - g.SLOP, g.pixelLeft);
	int R2LRearBumper = min(R2LFrontBumper + vehiclesR2L[R2LIndex].getBox().width + (5 * g.SLOP), g.pixelRight);  // 4x makes up for SLOP subtracted from front bumper.
	for (int L2RIndex = 0; L2RIndex < projectedL2RSize; L2RIndex++){
		int L2RRearBumper = vehiclesL2R[L2RIndex].getBox().x;
		int L2RFrontBumper = L2RRearBumper + vehiclesL2R[L2RIndex].getBox().width;
		if ( (R2LFrontBumper <= L2RFrontBumper) && (R2LFrontBumper >= L2RRearBumper)) frontOverlap = true;
		if ( ( (R2LRearBumper <= L2RFrontBumper) && (R2LRearBumper >= L2RRearBumper) ) 
		/*	|| ((R2LRearBumper >= L2RFrontBumper) && (R2LFrontBumper <= L2RRearBumper)) */  ) rearOverlap = true;   // Handle full obstruction case
	}
	// none, rearOnly, frontOnly, bothOverlap
	if (frontOverlap && rearOverlap) return bothOverlap;
	if (frontOverlap) return frontOnly;
	if (rearOverlap) return rearOnly;
	return none;
}



//  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  * 
// Given next differential image, use projections of all known in-track vehicles, as well as information about newly entering vehicles, to identify and process
// all that are 1) exiting, deleted, entering, overtaking, occluding, occluded, or simply moving forward.  If objects are detected in the region of interest, they
// are bracketed and associated with entering or already known vehicles, and this new information is preserved for each vehicle in a call to addSnap(), one call for
// each vehicle maintained in a direction sensitive vector of known to be in track vehicles.  The preservation of observed information enables predictive filter
// based tracking, done elsewhere.
//
//  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  * 

// The part of the motion image one direction's cars are looked for in: everything above that direction's hubcap line.  A
// level line (the usual case) is just the top rows, a view with no copy.  A line at an angle (L2RStreetYRight /
// R2LStreetYRight, Perspective.h) takes the rows down to its lower end and blacks out what lies below the slant, so the
// sidewalk under the line's high end can't join a car the way it would under a level line drawn at the low end.
static Mat laneView(const Mat& thresh, bool leftToRight){
	int bottom = min(g.laneBottom(leftToRight), thresh.rows);
	Mat view = thresh(Rect(0, 0, thresh.cols, bottom));
	if (!g.laneTilted(leftToRight)) return view;
	perspective::Line l = g.lane(leftToRight);
	Mat masked = view.clone();
	Point below[4] = { Point(0, int(lround(l.yLeft))), Point(thresh.cols, int(lround(l.at(thresh.cols)))), Point(thresh.cols, bottom), Point(0, bottom) };
	fillConvexPoly(masked, below, 4, Scalar(0));
	return masked;
}

bool manageMovers(Mat wholeScenethreshImage, Mat &AnalysisFrame){

/// < < < < < < < < < < < < < < < < < < < < < < < < < < G e t   P r o j e c t i o n s   f o r   v e h s   a l r e a d y   i n   t r a c k  > > > > > > > > > > > > > > > > 
// Get all L2R vehicle projections
	vector<Projection> projectedL2R;  // >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
	for (int index = 0; index < vehiclesGoingRight.size(); index++){
		projectedL2R.push_back(vehiclesGoingRight[index].getBestProjection(g, frameNumber));
		if (pleaseTrace) traceFile << endl << "<" << frameNumber << "> Project >>L2R>> vehicle[" << index << "]  Rect xywh: [" << projectedL2R[index].getBox().x << ", "
			<< projectedL2R[index].getBox().y << ", " << projectedL2R[index].getBox().width << ",  " << projectedL2R[index].getBox().height
			<< "]   vState: " << vStateString(projectedL2R[index].getVState())
			<< "  Overlap: " << overlapString(vehiclesGoingRight[index].getOverlapStatus())
			<< ",   pixDelta: " << projectedL2R[index].getVelocity() << endl
			<< "     FBSlope (+): " << vehiclesGoingRight[index].getFBSlope() << "   FBIntcpt:  " << vehiclesGoingRight[index].getFBIntercept()
			<< "  RBSlope: " << vehiclesGoingRight[index].getRBSlope() << "  RBIntcpt: " << vehiclesGoingRight[index].getRBIntercept()
			<< "      projected FB: " << int(vehiclesGoingRight[index].getNextFrontBumper())
			<< "  projected width: " << projectedL2R[index].getBox().width 
			<< "  projected RB: " << int(vehiclesGoingRight[index].getNextRearBumper())
			<< endl;
	}

// Get all R2L vehicle projections
	vector<Projection> projectedR2L;  // <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
	for (int index = 0; index < vehiclesGoingLeft.size(); index++){
		projectedR2L.push_back(vehiclesGoingLeft[index].getBestProjection(g, frameNumber));
		if (pleaseTrace) traceFile << endl << "<" << frameNumber << "> Project <<R2L<< vehicle[" << index << "]  Rect xywh: [" << projectedR2L[index].getBox().x << ", "
			<< projectedR2L[index].getBox().y << ", " << projectedR2L[index].getBox().width << ",  " << projectedR2L[index].getBox().height
			<< "]   vState: " << vStateString(projectedR2L[index].getVState())
			<< "  Overlap: " << overlapString(vehiclesGoingLeft[index].getOverlapStatus())
			<< ",   pixDelta: " << projectedR2L[index].getVelocity() << endl
			<< "     FBSlope (-): " << vehiclesGoingLeft[index].getFBSlope() << "   FBIntcpt:  " << vehiclesGoingLeft[index].getFBIntercept()
			<< "  RBSlope: " << vehiclesGoingLeft[index].getRBSlope() << "  RBIntcpt: " << vehiclesGoingLeft[index].getRBIntercept()
			<< "      projected FB: " << int(vehiclesGoingLeft[index].getNextFrontBumper())
			<< "  projected width: " << projectedR2L[index].getBox().width 
			<< "      projected RB: " << int(vehiclesGoingLeft[index].getNextRearBumper())
			<< endl;
	}


//  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  Get rid of all exited and deleted vehicles *  *  *  *  *  *  *  *  *  * 

// If front L2R vehicle is exited, remove it from consideration
	if ((vehiclesGoingRight.size() > 0) && (projectedL2R.front().getVState() == exited)){
		if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # L2R vehicle just exited." << endl;
//		cout << "<" << frameNumber << ">   # # # # # # # L2R vehicle just exited." << endl;
		logL2Rstats(true, 0); //
		vehiclesGoingRight.erase(vehiclesGoingRight.begin());
		projectedL2R.erase(projectedL2R.begin());
	}

// If front R2L vehicle is exited, remove it from consideration
	if ((vehiclesGoingLeft.size() > 0) && (projectedR2L.front().getVState() == exited)){
		if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # R2L vehicle just exited." << endl;
//		cout << "<" << frameNumber << ">   # # # # # # # R2L vehicle just exited." << endl;
		logR2Lstats(true, 0);
		vehiclesGoingLeft.erase(vehiclesGoingLeft.begin());
		projectedR2L.erase(projectedR2L.begin());
	}


// Check for deleted L2R vehicles

	for (int index = vehiclesGoingRight.size() - 1; index > -1; index--){
		if (vehiclesGoingRight[index].getAmIOK() != ImOK) {
			if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # L2R vehicle[" << index << "] is being deleted: " << statusString(vehiclesGoingRight[index].getAmIOK()) << endl;
//			cout << "<" << frameNumber << ">   # # # # # # # L2R vehicle[" << index << "] is being deleted: " << statusString(vehiclesGoingRight[index].getAmIOK()) << endl;
			if (vehiclesGoingRight[index].getTrackEndPixel() > 0) logL2Rstats(true, index);
			else logL2Rstats(false, /*vehiclesGoingRight[index].getAmIOK() == deleteWithStats*/ index);
			vehiclesGoingRight.erase(vehiclesGoingRight.begin() + index);
			projectedL2R.erase(projectedL2R.begin() + index);
		}
	}

// Check for deleted R2L vehicles

	for (int index = vehiclesGoingLeft.size() - 1; index > -1; index--){
		if (vehiclesGoingLeft[index].getAmIOK() != ImOK) {
			if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # R2L vehicle[" << index << "] is being deleted: " << statusString(vehiclesGoingLeft[index].getAmIOK()) << endl;
//			cout << "<" << frameNumber << ">   # # # # # # # R2L vehicle[" << index << "] is being deleted: " << statusString(vehiclesGoingLeft[index].getAmIOK()) << endl;
			if (vehiclesGoingLeft[index].getTrackEndPixel() > 0) logR2Lstats(true, index);
			else logR2Lstats(false, /*vehiclesGoingLeft[index].getAmIOK() == deleteWithStats,*/ index);
			vehiclesGoingLeft.erase(vehiclesGoingLeft.begin() + index);
			projectedR2L.erase(projectedR2L.begin() + index);
		}
	}

// Check for L2R overrunning, as in a vehicle starting to pass a bicyclist; bail if overrunning detected.
// This could be modified to delete the overrun vehicle instead, but leapfrogging would have to be dealt with.

	for (int index = vehiclesGoingRight.size() - 1; index > 0; index--){
		if (index > 0 && (projectedL2R[index].getBox().x + projectedL2R[index].getBox().width) > (projectedL2R[index - 1].getBox().x - g.margin(200, true)) ) {
			if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # L2R vehicle[" << index << "] is being deleted for overrunning: " << endl;
			cout << "<" << frameNumber << ">   # # # # # # # L2R vehicle[" << index << "] is overrunning: "  << endl;
			// For now, erase all ongoing vehicle records and wait for scene to go quiescent.  Then start analyzing again.
			dropTracks();  // erases both directions, counting those already in the speed zone as untimed
			projectedL2R.erase(projectedL2R.begin(), projectedL2R.end());
			projectedR2L.erase(projectedR2L.begin(), projectedR2L.end());
			bailing = true;
			cv::line(AnalysisFrame, Point(g.pixelLeft + 1, 20), Point(g.pixelRight - 1, 20), Scalar(CVRed), 2);
			if (pleaseTrace) traceFile << "<" << frameNumber << "> Starting to bail because of overrunning.   All current vehicles being dropped." << endl;
		}
	}

// Check for R2L overrunning, as in a vehicle starting to pass a bicyclist; bail if overrunning detected.
// This could be modified to delete the overrun vehicle instead, but leapfrogging would have to be dealt with.

	for (int index = vehiclesGoingLeft.size() - 1; index > 0; index--){
		if (index > 0 && ((projectedR2L[index - 1].getBox().x + projectedR2L[index - 1].getBox().width) > (projectedR2L[index].getBox().x - g.margin(200, false)))) {
			if (pleaseTrace) traceFile << endl << "<" << frameNumber << ">   # # # # # # # R2L vehicle[" << index << "] is being deleted for overrunning: " << endl;
			cout << "<" << frameNumber << ">   # # # # # # # R2L vehicle[" << index << "] is overrunning: " << endl;
			// For now, erase all ongoing vehicle records and wait for scene to go quiescent.  Then start analyzing again.
			dropTracks();  // erases both directions, counting those already in the speed zone as untimed
			projectedL2R.erase(projectedL2R.begin(), projectedL2R.end());
			projectedR2L.erase(projectedR2L.begin(), projectedR2L.end());
			bailing = true;
			cv::line(AnalysisFrame, Point(g.pixelLeft + 1, 20), Point(g.pixelRight - 1, 20), Scalar(CVRed), 2);
			if (pleaseTrace) traceFile << "<" << frameNumber << "> Starting to bail because of overrunning.   All current vehicles being dropped." << endl;
		}
	}
	


//  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  Detect places of motion  *  *  *  *  *  *  *  *  *  * 
	vector< vector<Point> > contours; // for findContours output
	vector<Vec4i> hierarchy;  // for findContours output

	// Each direction looks only above its own hubcap line (see laneView()).
	const Mat threshL2R = laneView(wholeScenethreshImage, true), threshR2L = laneView(wholeScenethreshImage, false);
	int numOKSizeObjectsL2R = 0; // Used to count how many detected objects are in selected region of interest
	Mat tempForL2R = threshL2R(Rect(g.pixelLeft, 0, g.pixelRight, threshL2R.rows)); // L2RStreetY is the lowest needed to go to see a rightbound vehicle
	Rect objectBoundingRectangleL2R[MAX_NUM_OBJECTS]; // bounding rectangles, from top of ROI to L2R lane, captured in a given frame

	findContours(tempForL2R, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);// retrieves external contours
	// found some objects?
	if (contours.size() > 0){   // Are both of
		if (hierarchy.size() > 0) {  // these necessary?
			numObjects = hierarchy.size();
			//if number of objects greater than MAX_NUM_OBJECTS may need to adjust noise filter
			if (numObjects < MAX_NUM_OBJECTS){
				for (int index = 0; index >= 0; index = hierarchy[index][0]) {
					objectBoundingRectangleL2R[numOKSizeObjectsL2R] = boundingRect(contours.at(index)); 		//make bounding rectangle 
					if ((objectBoundingRectangleL2R[numOKSizeObjectsL2R].width * objectBoundingRectangleL2R[numOKSizeObjectsL2R].height) >= MIN_OBJECT_AREA)
						numOKSizeObjectsL2R++;
				} // for
			} // if
			else {
				cout << "Too many L2R objects!" << endl;
				numOKSizeObjectsL2R = 0;
			}
		} // if
	} // if


	int numOKSizeObjectsR2L = 0; // Used to count how many detected objects are in selected region of interest
	Mat tempForR2L = threshR2L(Rect(g.pixelLeft, 0, g.pixelRight, threshR2L.rows)); // R2LStreetY is the lowest needed to go for leftbound vehicle
	Rect objectBoundingRectangleR2L[MAX_NUM_OBJECTS]; // bounding rectangles captured in a given frame

	contours.erase(contours.begin(), contours.end()); // Clear contours vector
	hierarchy.erase(hierarchy.begin(), hierarchy.end());  // Clear hierarchy vector
	//find external contours of filtered image using openCV findContours function
	findContours(tempForR2L, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);// retrieves external contours
	// found some objects?
	if (contours.size() > 0){   // Are both of
		if (hierarchy.size() > 0) {  // these necessary?
			numObjects = hierarchy.size();
			//if number of objects greater than MAX_NUM_OBJECTS may need to adjust noise filter
			if (numObjects < MAX_NUM_OBJECTS){
				for (int index = 0; index >= 0; index = hierarchy[index][0]) {
					objectBoundingRectangleR2L[numOKSizeObjectsR2L] = boundingRect(contours.at(index)); 		//make bounding rectangle 
					if ((objectBoundingRectangleR2L[numOKSizeObjectsR2L].width * objectBoundingRectangleR2L[numOKSizeObjectsR2L].height) >= MIN_OBJECT_AREA)
						numOKSizeObjectsR2L++;
				} // for
			} // if
			else {
				cout << "Too many R2L objects!" << endl;
				numOKSizeObjectsR2L = 0;
			}
		} // if
	} // if



// At this point objectBoundingRectanglexxx[] has numOKSizeObjectsxxx acceptable rectangles in it, possibly zero.  The rectangles are independent, non-overlapping.


// This bailing code is used in circumstances where the scene is overwhelming.

	if (((numOKSizeObjectsL2R + numOKSizeObjectsR2L) > 0) && bailing){
		cv::line(AnalysisFrame, Point(g.pixelLeft + 1, 20), Point(g.pixelRight - 1, 20), Scalar(CVRed), 2);
		if (pleaseTrace) traceFile << "<" << frameNumber << "> Still bailing." << endl;
		return true;
	}
	else if(bailing){ // bailing with no objects detected.
		bailing = false;
		if (pleaseTrace) traceFile << "<" << frameNumber << "> Returning to analyzing traffic." << endl;
	}



//  Visual bracketing of the left/right ends of the range where speed measuring takes place.

	cv::line(AnalysisFrame, Point(g.speedLineLeft, 180), Point(g.speedLineLeft, 25), Scalar(CVWhite), 2);
	cv::line(AnalysisFrame, Point(g.speedLineRight, 180), Point(g.speedLineRight, 25), Scalar(CVWhite), 2);


	if ((numOKSizeObjectsL2R + numOKSizeObjectsR2L) > 0) { // rectangles found in areas checked, i.e. motion detected;  See what's up...

//		cout << "<" << frameNumber << "> Num OK objects: " << numOKSizeObjects << "  L2R vehicles: " << vehiclesGoingRight.size() << "  R2L vehicles: " << vehiclesGoingLeft.size() << endl;
		if (pleaseTrace) traceFile << "<" << frameNumber << "> Num OK L2R objects: " << numOKSizeObjectsL2R
			<< " Num OK R2L objects: " << numOKSizeObjectsR2L << "  L2R vehicles: " << vehiclesGoingRight.size() << "  R2L vehicles: " << vehiclesGoingLeft.size() << endl;


//  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  *  Safe to look for newly entering vehicles at the left and right ends of the analysis box? *  *  *  *  *  *  *  *
//                                                       =========================================================================================

// Left end is safe to check if there are no entering L2R vehicles and no R2L vehicles w/in a couple of frames of exiting left.
//  > > > > > > > > > > > > > > > > > 
		int safeL2RZone = g.pixelRight - g.pixelLeft;
		int safeR2LZone = g.pixelRight - g.pixelLeft;
		// "6" and "2" in following if statements can be tweaked.  I'm happy with their current values.
		if (projectedR2L.size() > 0) safeR2LZone = max(projectedR2L.front().getBox().x - (2 * g.maxL2RDistOnEntry), 0);  // Identify safe range to front of oncoming car.
		if (projectedL2R.size() > 0) safeL2RZone = max(projectedL2R.back().getBox().x - (6 * g.maxL2RDistOnEntry), 0);  // Identify safe range to rear of preceding car.

		if (safeL2RZone > 0 && safeR2LZone > 0){
			coalescedRectangle = coalesce(objectBoundingRectangleL2R, numOKSizeObjectsL2R,
				g.pixelLeft, min(min(safeR2LZone, safeL2RZone), (g.pixelLeft + g.pixelRight) / 2), strict);  // Look for vehicle from left (-20 covers projection slop)
			if (coalescedRectangle.x != -1){ // at least one object is present in coalesced rectangle(s)
				vehiclesGoingRight.push_back(VehicleDynamics(L2R));
				vehiclesGoingRight[vehiclesGoingRight.size() - 1].addSnapshot(Snapshot(coalescedRectangle, frameNumber));
				// First seen already in the zone: usually a lost track picked up again.  In a short view (subFrameTiming) a car can be a
				// frame pair into the zone before it is first seen, so one still at the left edge is just entering and is timed from there.
				if (coalescedRectangle.x + coalescedRectangle.width >= g.speedLineLeft && !(g.subFrameTiming && coalescedRectangle.x <= g.margin(45, true)))
					     vehiclesGoingRight[vehiclesGoingRight.size() - 1].markInvalidSpeed();
				if (pleaseTrace) traceFile << endl << endl << "<" << frameNumber
					<< ">    >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> Just added rightbound vehicle[" << vehiclesGoingRight.size() - 1 << "] >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << endl;
				displayAnalysisGoingRight(frameNumber, vehiclesGoingRight.size() - 1, coalescedRectangle, none, AnalysisFrame, -1);
			}
		}

// Right end is safe if there are no entering R2L vehicles and no L2R vehicles w/in a couple of frames of exiting left.
// < < < < < < < < < < < < < < < < < < 
		safeL2RZone = g.pixelRight - g.pixelLeft;
		safeR2LZone = g.pixelRight - g.pixelLeft;
		 // "6" and "2" in following if statements can be tweaked.  I'm happy with their current values.
		if (projectedR2L.size() > 0)  safeR2LZone = max(g.pixelRight - (projectedR2L.back().getBox().x + projectedR2L.back().getBox().width + (6 * g.maxR2LDistOnEntry)), 0);   // Identify safe range to rear of preceding car.
		if (projectedL2R.size() > 0) safeL2RZone = max(g.pixelRight - (projectedL2R.front().getBox().x + projectedL2R.front().getBox().width + (2 * g.maxR2LDistOnEntry)), 0);  // Identify safe range to front of oncoming car.

		if (safeR2LZone > 0 && safeL2RZone > 0){
			coalescedRectangle = coalesce(objectBoundingRectangleR2L, numOKSizeObjectsR2L,
				     max(  max(g.pixelRight - safeL2RZone, g.pixelRight - safeR2LZone),
				          (g.pixelLeft + g.pixelRight) / 2), g.pixelRight, strict);  // Look for vehicle from right
			if (coalescedRectangle.x != -1){
				vehiclesGoingLeft.push_back(VehicleDynamics(R2L));
				vehiclesGoingLeft[vehiclesGoingLeft.size() - 1].addSnapshot(Snapshot(coalescedRectangle, frameNumber));
				if (coalescedRectangle.x <= g.speedLineRight  // see the note on L2R just above
					&& !(g.subFrameTiming && coalescedRectangle.x + coalescedRectangle.width >= g.pixelRight - g.margin(45, false)))
					     vehiclesGoingLeft[vehiclesGoingLeft.size() - 1].markInvalidSpeed();
				if (pleaseTrace) traceFile << endl << endl << "<" << frameNumber
					<< ">     <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<< Just added leftbound vehicle[" << vehiclesGoingLeft.size() - 1 << "] <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<" << endl;
				displayAnalysisGoingLeft(frameNumber, vehiclesGoingLeft.size() - 1, coalescedRectangle, none, AnalysisFrame, -1);
			}
		}


// * * * * * * * * * * * * * * * * * * * * * * * *  P r o c e s s    a l l    p r o j e c t e d    v e h i c l e s * * * * * * * * * * * * * * * * * * * *
//                                                 ================================================================

// ---------------More than three in analysis zone with opposing traffic......
		if ((vehiclesGoingRight.size() * vehiclesGoingLeft.size()) > 2){ // Bail on mixed direction, four or more vehicles total (includes just entered vehs)
			// For now, erase all ongoing vehicle records and wait for scene to go quiescent.  Then start analyzing again.
			dropTracks();  // erases both directions, counting those already in the speed zone as untimed
			projectedL2R.erase(projectedL2R.begin(), projectedL2R.end());
			projectedR2L.erase(projectedR2L.begin(), projectedR2L.end());
			bailing = true;
			cv::line(AnalysisFrame, Point(g.pixelLeft + 1, 20), Point(g.pixelRight - 1, 20), Scalar(CVRed), 2);
			if (pleaseTrace) traceFile << "<" << frameNumber << "> Starting to bail because  > 3 bi-directional traffic detected.  All current vehicles being dropped." << endl;
		}


		else {  // Ok, scene is one that can be handled. Clear past info about passing vehicles, and then check for passing vehicles now.
			if(vehiclesGoingRight.size() > 0)
				for (int i = 0; i < vehiclesGoingRight.size(); i++) 
					vehiclesGoingRight[i].setOverlapStatus(none); // Reset any past L2R overlap determinations.
			if (vehiclesGoingLeft.size() > 0)
				for (int i = 0; i < vehiclesGoingLeft.size(); i++)
					vehiclesGoingLeft[i].setOverlapStatus(none); // Reset any past R2L overlap determinations.
// -------------- Two or three vehicles in analysis zone, a least one in each direction......
			if ((projectedL2R.size() * projectedR2L.size()) >= 1) { // Two vehs currently in track (ignoring just entered vehicles now), one in each direction;
				for (int i = 0; i < projectedL2R.size(); i++)
					vehiclesGoingRight[i].setOverlapStatus(doesL2ROverlapAnyR2L(i, projectedL2R, projectedR2L, projectedR2L.size()));
				for (int i = 0; i < projectedR2L.size(); i++)
					vehiclesGoingLeft[i].setOverlapStatus(doesR2LOverlapAnyL2R(i, projectedR2L, projectedL2R, projectedL2R.size()));
			}

		}

// > > > > > > > > > > > >  All *current* vehicles from left case (any newly added L2R vehicle not considered) > > > > > > > > > > > > > > > > > > > 
//		                   ===================================================================================
//  > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > > >

		Rect objectBoundingRectangle[MAX_NUM_OBJECTS]; // bounding rectangles, formed in a direction sensitive manner

		if (projectedL2R.size() > 0){ // All bidirectional cases considered by the time control gets here.
			for (int index = 0; index < projectedL2R.size(); index++){
            // First, focus the search for detected blobs to the region the vehicle is projected to occupy
				//find external contours of filtered image using openCV findContours function
				contours.erase(contours.begin(), contours.end()); // Clear contours vector
				hierarchy.erase(hierarchy.begin(), hierarchy.end());  // Clear hierarchy vector
				int tempX = max(projectedL2R[index].getBox().x - g.margin(80, true), g.pixelLeft);  // look behind the predicted rear bumper
				int tempWidth = min(projectedL2R[index].getBox().width + g.margin(100, true), g.pixelRight - tempX); // Look a little beyond the front bumper;
				Mat ROIL2R = threshL2R(Rect(tempX, 0, tempWidth, threshL2R.rows));
				findContours(ROIL2R, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);// retrieves external contours
				int numOKSizeL2RObjects = 0; // Used to count how many detected objects are in selected region of interest
				// found some objects?
				if (contours.size() > 0){   // Are both of
					if (hierarchy.size() > 0) {  // these necessary?
						for (int index = 0; index >= 0; index = hierarchy[index][0]) {
							objectBoundingRectangle[numOKSizeL2RObjects] = boundingRect(contours.at(index)); 		//make bounding rectangle 
							if ((objectBoundingRectangle[numOKSizeL2RObjects].width * objectBoundingRectangle[numOKSizeL2RObjects].height) >= MIN_OBJECT_AREA){
								objectBoundingRectangle[numOKSizeL2RObjects].x += tempX;
								numOKSizeL2RObjects++;
							}
						} // for
					} // if
				} // if
			if(pleaseTrace) traceFile << "    Number of L2R objects is: " << numOKSizeL2RObjects << "  inside rect[x,y,wid,ht] "
				<< tempX << ", " << 0 << ", " << tempWidth << ", " << g.L2RStreetY << endl;

			// Get the best bounding rectangle possible for the vehicle being considered; if no objects were found, skip to display of projected data
				if (numOKSizeL2RObjects > 0){
					int projFrontBumper = projectedL2R[index].getBox().x + projectedL2R[index].getBox().width;
					int projRearBumper = projectedL2R[index].getBox().x;
					grabType grabRestriction = greedy;
					if (vehiclesGoingRight[index].getOverlapStatus() == rearOnly)
						grabRestriction = strict;
					if (projectedL2R[index].getVState() == entering)
						// Look a few pixels beyond projections in each direction
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeL2RObjects, max(g.pixelLeft, (projRearBumper - g.margin(10, true))), (projFrontBumper + g.margin(20, true)), grabRestriction);
					else if (projectedL2R[index].getVState() == exiting)  // Look a few pixels beyond projections in each direction
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeL2RObjects, (projRearBumper - g.margin(10, true)), min((projFrontBumper + g.margin(10, true)), g.pixelRight), grabRestriction);
					else  // somewhere in the middle
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeL2RObjects, max((projRearBumper - g.margin(50, true)),
						g.pixelLeft), min((projFrontBumper + g.margin(10, true)), g.pixelRight), grabRestriction);

					// If no coalesced objects have been found, record no snapshot.
					//                                        =======================
					if (coalescedRectangle.x >= 0){    					// Coalesced objects found...
						vehiclesGoingRight[index].addSnapshot(Snapshot(coalescedRectangle, frameNumber));
						// draw a purple rectangle around the area where objects related to the vehicle were found ("actual data")
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y), Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y), Scalar(CVPurple), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y + coalescedRectangle.height),
							Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVPurple), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y), Point(coalescedRectangle.x, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVPurple), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y),
							Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVPurple), 2);
						if (pleaseTrace) traceFile << "  Observed FB: " << coalescedRectangle.x + coalescedRectangle.width
							<< "   Observed width: " << coalescedRectangle.width 
							<< "  Observed RB: " << coalescedRectangle.x
							<< endl;
					}
				}

				displayAnalysisGoingRight(frameNumber, index, projectedL2R[index].getBox(), vehiclesGoingRight[index].getOverlapStatus(), AnalysisFrame, vehiclesGoingRight[index].getFinalSpeed());
			}
		}

//  < < < < < < < < < < < < < < < <   All *current* vehicles from right case (any newly added R2L vehicle not considered) < < < < < < < < < < < < < < < < < < < < < <
//		                              ===================================================================================
//  < < < < < < < < < < < < < < < <  < < < < < < < < < < < < < < < <  < < < < < < < < < < < < < < < <  < < < < < < < < < < < < < < < <  < < < < < < < < < < < < < < < < 


		if (0 < projectedR2L.size()) { 
			for (int index = 0; index < projectedR2L.size(); index++){
				// First, focus the search for detected blobs to the region the vehicle is projectyed to occupy
				//find external contours of filtered image using openCV findContours function
				contours.erase(contours.begin(), contours.end()); // Clear contours vector
				hierarchy.erase(hierarchy.begin(), hierarchy.end());  // Clear hierarchy vector
				int tempX = max(projectedR2L[index].getBox().x - g.margin(20, false), g.pixelLeft);  // look a little ahead of the predicted front bumper
				int tempWidth = min(projectedR2L[index].getBox().width + g.margin(100, false), g.pixelRight - tempX); // Look behind the rear bumper;
				Mat ROIR2L = threshR2L(Rect(tempX, 0, tempWidth, threshR2L.rows));
				findContours(ROIR2L, contours, hierarchy, RETR_EXTERNAL, CHAIN_APPROX_SIMPLE);// retrieves external contours
				int numOKSizeR2LObjects = 0; // Used to count how many detected objects are in selected region of interest
				// found some objects?
				if (contours.size() > 0){   // Are both of
					if (hierarchy.size() > 0) {  // these necessary?
						for (int index = 0; index >= 0; index = hierarchy[index][0]) {
							objectBoundingRectangle[numOKSizeR2LObjects] = boundingRect(contours.at(index)); 		//make bounding rectangle 
							if ((objectBoundingRectangle[numOKSizeR2LObjects].width * objectBoundingRectangle[numOKSizeR2LObjects].height) >= MIN_OBJECT_AREA){
								objectBoundingRectangle[numOKSizeR2LObjects].x += tempX;
								numOKSizeR2LObjects++;
							}
						} // for
					} // if
				} // if
				if (pleaseTrace) traceFile << "    Number of R2L objects is: " << numOKSizeR2LObjects << "  inside rect[x,y,wid,ht] "
					<< tempX << ", " << 0 << ", " << tempWidth << ", " << g.R2LStreetY << endl;



				// Get the best bounding rectangle possible
				if (numOKSizeR2LObjects > 0){ // Get the best bounding rectangle possible for the vehicle being considered; if no objects were found, skip to display of projected data
					int projFrontBumper = projectedR2L[index].getBox().x;
					int projRearBumper = projFrontBumper + projectedR2L[index].getBox().width;
					grabType grabRestriction = greedy;
					if (vehiclesGoingLeft[index].getOverlapStatus() == rearOnly)
						grabRestriction = strict;
					if (projectedR2L[index].getVState() == entering)
						// Look a few pixels beyond projections in each direction
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeR2LObjects, (projFrontBumper - g.margin(20, false)), min((projRearBumper + g.margin(10, false)), g.pixelRight), grabRestriction); // minus for R2L vehicle
					else if (projectedR2L[index].getVState() == exiting)  // Look a few pixels beyond projections in each direction
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeR2LObjects, max((projFrontBumper - g.margin(10, false)), g.pixelLeft), (projRearBumper + g.margin(10, false)), grabRestriction);
					else  // somewhere in the middle
						coalescedRectangle = coalesce(objectBoundingRectangle, numOKSizeR2LObjects, max((projFrontBumper - g.margin(20, false)), g.pixelLeft),
						min(projRearBumper + g.margin(50, false), g.pixelRight), grabRestriction);

					// If no coalesced objects have been found, record no snapshot.
					//                                        =======================
					if (coalescedRectangle.x >= 0){  					// Coalesced objects found...
						vehiclesGoingLeft[index].addSnapshot(Snapshot(coalescedRectangle, frameNumber));
						// draw an orange rectangle around the area where objects related to the vehicle were found ("actual data")
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y), Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y), Scalar(CVOrange), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y + coalescedRectangle.height),
							Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVOrange), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x, coalescedRectangle.y), Point(coalescedRectangle.x, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVOrange), 2);
						line(AnalysisFrame, Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y),
							Point(coalescedRectangle.x + coalescedRectangle.width, coalescedRectangle.y + coalescedRectangle.height), Scalar(CVOrange), 2);
						if (pleaseTrace) traceFile << "  Observed FB: " << coalescedRectangle.x
							<< "   Observed width: " << coalescedRectangle.width 
							<< "   Observed RB: " << coalescedRectangle.x + coalescedRectangle.width
							<< endl;
					}
				}
				displayAnalysisGoingLeft(frameNumber, index, projectedR2L[index].getBox(), vehiclesGoingLeft[index].getOverlapStatus(), AnalysisFrame, vehiclesGoingLeft[index].getFinalSpeed());

			}
		}
	}

	else; // cout << "<" << frameNumber << ">  . . ." << endl; // This happens if numOKObjects == 0;

	return ((numOKSizeObjectsL2R + numOKSizeObjectsR2L) > 0);
}


// ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^  M a i n  ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ 
// ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^  M a i n  ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ ^ 
//
//  Process image data per user request.  Key call halfway down is this:
//                                                    objectDetected = manageMovers(thresholdImage, ROIFr2);
// which causes processing of all known and newly entered vehicls to occur at time "frameNumber."  This one call exercises most of the code above
// and most all of the code in vehicleDynamics.

void printSummary(){
	cout << loggedVehicles << " vehicles logged, " << speeders << " over " << speedLimit << " MPH";
	if (g.LegalDirection != "Both") cout << ", " << wrongWayVehicles << " going the wrong way";
	cout << endl;
}

// ................................................ Live sources: riding out dropouts ................................................
// A camera or stream left running for days will drop out now and then: a Wi-Fi blip, a camera reboot, a router restart.
// For a file the first failed read is its end, and the job ends there as it always has.  A live source is instead reopened
// after a growing wait (reconnectWaits), for as long as it takes or until the run is stopped.  Vehicles being tracked when the
// picture stopped are dropped, so no speed is ever measured across a gap, and frames keep counting from where they were so
// the csv Frame column never goes backwards.
const int reconnectWaits[] = { 1, 2, 4, 8, 16, 30 };  // seconds before each reopen attempt; the last one repeats forever
int reconnectAttempts = 0;        // attempts since frames last arrived; 0 while the source is delivering
double outageStarted = 0;         // clockNow() when the current outage began
bool stoppedWhileReconnecting = false;

// Tells an embedding host (VST Studio's "Reconnecting..." badge) that a live source went down or came back.
void notifySource(bool reconnecting, int attempt, int waitSeconds, const string& message){
#ifdef VST_EMBEDDED
	if (hooks && hooks->onSourceStatus) hooks->onSourceStatus({ reconnecting, attempt, waitSeconds, message });
#else
	(void)reconnecting; (void)attempt; (void)waitSeconds; (void)message;
#endif
}

// Sleeps for ms, checking every 100 ms for Esc (or, embedded, the host's Stop button, which pollKey reports as Esc).
// Returns true if the user asked to stop.
bool waitOrStop(int ms){
	for (auto until = chrono::steady_clock::now() + chrono::milliseconds(ms); chrono::steady_clock::now() < until;){
		if (uiWaitKey(100) == 27) return true;
		if (headless) this_thread::sleep_for(chrono::milliseconds(100));  // without a window uiWaitKey() returns at once
	}
	return false;
}

// Reads the next frame pair into frame1/frame2.  Returns false at the end of a file, or when the user stops a live
// source while it is reconnecting; otherwise a live source that stops delivering is reopened here until it delivers again.
bool readFramePair(const string& source){
	for (;;){
		if (readFrame(*capture, frame1) && readFrame(*capture, frame2)){
			if (reconnectAttempts > 0){
				string line = "Reconnected to " + sourceLabel(source) + " at " + formatTime(clockNow(), false) + " after a "
					+ to_string(int(clockNow() - outageStarted)) + " s gap (" + to_string(reconnectAttempts) + " attempts).  Tracking restarted.";
				cout << "<" << frameNumber << ">  " << line << endl;
				if (pleaseTrace) traceFile << "<" << frameNumber << "> " << line << endl;
				notifySource(false, 0, 0, line);
				reconnectAttempts = 0;
				// The gap is over: put its end in the manifest now, so it is on disk even if the run never ends cleanly.
				if (!outages.empty()) outages.back().to = clockNow();
				writeManifest(false);
			}
			return true;
		}
		if (!liveSource) return false;  // end of the file
		if (reconnectAttempts == 0){
			// Start of an outage.  Whatever was in the speed zone can't be finished honestly: drop it, and start
			// tracking afresh on the first frames after the gap.
			outageStarted = clockNow();
			cout << "<" << frameNumber << ">  Lost " << sourceLabel(source) << " at " << formatTime(outageStarted, false)
				<< "; vehicles being tracked, now dropped: " << vehiclesGoingRight.size() + vehiclesGoingLeft.size() << endl;
			if (pleaseTrace) traceFile << "<" << frameNumber << "> Source lost; tracks dropped" << endl;
			dropTracks();
			bailing = false;
			// For the manifest the gap starts at the last frame that arrived (runEnded, this computer's clock for a live
			// source), which can be several seconds before the failed read noticed it.
			outages.push_back({ runEnded >= 0 ? runEnded : outageStarted, -1 });
			numObjects = 0;
		}
		capture->release();
		int wait = reconnectWaits[min(reconnectAttempts, int(size(reconnectWaits)) - 1)];
		reconnectAttempts++;
		string line = "Reconnecting to " + sourceLabel(source) + ": attempt " + to_string(reconnectAttempts) + " in " + to_string(wait) + " s";
		cout << "<" << frameNumber << ">  " << line << endl;
		notifySource(true, reconnectAttempts, wait, line);
		if (waitOrStop(wait * 1000)){
			cout << "Stopped while reconnecting." << endl;
			stoppedWhileReconnecting = true;
			return false;
		}
		manifestCheckpoint();  // a long outage still gets its 10-minute rewrites
		// Opened or not, the next pass round the loop decides: a source that opens but sends no frames still counts as down,
		// so the waits keep growing instead of hammering a camera that is half awake.
		if (!openSource(capture, source)) cout << "<" << frameNumber << ">  Couldn't open " << sourceLabel(source) << endl;
	}
}

int vstMain(int argc, char** argv){

	bool objectDetected = false;
	bool pause = false;  	 // toggle using "p"
	Mat grayImage1, grayImage2; // for absdiff() function
	Mat differenceImage;
	Mat thresholdImage;  	//thresholded difference image (for use in findContours() function)

#ifndef VST_EMBEDDED
	userpaths::holdConsoleOnExit();  // Windows: a double-clicked exe keeps its window open to show why it stopped
#endif
	if (argc > 1){
		if (!setupFromArgs(argc, argv)) return EXIT_FAILURE;  // Non-interactive: everything comes from the command line
	}
	else {
		cout << "VideoSpeedTracker interactive mode.  Run VideoSpeedTracker --help for command-line options." << endl;
		setup();  // Get config data and user preferences for files to process, tracing, debugging, start frame and others
	}
	bool showVideo = !headless;  // turning this off should make processing run faster.  toggled with a "v"

	// What the run manifest says was analyzed: the file's name, the URL without passwords, or the camera index.  A "*" run
	// in interactive mode (all the videos of one folder into one csv) is named by its folder.
	const string& first = filesToDo[0];
	manifestKind = isLiveCamera(first) ? "camera" : isStreamUrl(first) ? "stream" : "file";
	if (filesToDo.size() > 1) manifestInput = fs::path(first).parent_path().filename().string();
	else manifestInput = manifestKind == "file" ? fs::path(first).filename().string() : withoutPassword(first);

//  * * * * * * * * * * * * * * * * * * * * * *  M a i n   L o o p   o v e r   o n e   o r   m o r e   i n p u t   f i l e s  * * * * * * * * * * * * * * * *


	for (size_t fileIndex = 0; fileIndex < filesToDo.size(); fileIndex++){

		string FName = filesToDo[fileIndex];
		fileName = fs::path(FName).filename().string();
		if (fileIndex > 0){  // Each further file of a "*" run starts from scratch at frame 0
			vehiclesGoingRight.clear();
			vehiclesGoingLeft.clear();
			bailing = false;
			startFrame = 0.0;
		}
		loggedVehicles = speeders = wrongWayVehicles = 0;
		cout << endl << "Now processing " << withoutPassword(FName) << endl;
		if (pleaseTrace) traceFile << endl << "Now processing cam input file: " << fileName << endl;
		lastPiError.clear();
		openSource(capture, FName);

		if (!capture->isOpened()){
			if (isPiCamera(FName)) cerr << "Can't start " << sourceLabel(FName) << " (the reason is above)." << endl;
			else cerr << "Can't open video " << withoutPassword(FName) << endl
				<< "Check the path, and that this OpenCV build can decode it (try converting with: ffmpeg -i in.ext -c:v libx264 out.mp4)." << endl;
			return EXIT_FAILURE;
		}

		// Any resolution works: readFrame() scales to 1280x720.  Any frame rate works: computeFinalSpeed() rescales the
		// calibration frame counts from CalibrationFPS to this rate.  Below ~20 fps cars jump too far between frames to track well.
		int width = int(capture->get(CAP_PROP_FRAME_WIDTH)), height = int(capture->get(CAP_PROP_FRAME_HEIGHT));
		if (width != Globals::FrameWidth || height != Globals::FrameHeight)
			cout << "Scaling " << width << "x" << height << " frames to " << Globals::FrameWidth << "x" << Globals::FrameHeight
				<< "; VST.cfg pixel values refer to the scaled frame." << endl;
		double FPS = capture->get(CAP_PROP_FPS);
		bool fpsReported = FPS > 0 && FPS < 1000;
		if (!fpsReported){
			cout << "Video doesn't report a usable frame rate; assuming CalibrationFPS (" << g.CalibrationFPS << ")." << endl;
			FPS = g.CalibrationFPS;
		}
		else if (fabs(FPS - g.CalibrationFPS) > 0.01)
			cout << "Video is " << FPS << " fps and calibration was counted at " << g.CalibrationFPS << " fps; speeds are scaled to match." << endl;
		if (FPS < 20)
			cout << "Warning: " << FPS << " fps is low for tracking; 30 fps is recommended." << endl;
		g.videoFPS = FPS;
		// Live means a camera, or a URL that doesn't report a length.  A URL that does (an http:// link to a recorded file)
		// is a file: it ends at its end instead of being reopened and played again.
		liveSource = isLiveCamera(FName) || (isStreamUrl(FName) && !(capture->get(CAP_PROP_FRAME_COUNT) > 0));
		reconnectAttempts = 0;
		stoppedWhileReconnecting = false;
		setTimeBase(FName, fpsReported);

		capture->set(CAP_PROP_POS_FRAMES, startFrame);  // Set frame number to start at, in first file to be processed;  Remaining files will start at zero.
		frameNumber = int(startFrame);
		int delay = 10;   //at least 10ms delay is necessary for proper operation of this program <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

		//work through frame pairs looking for differences, until the video runs out.  Reading (rather than trusting
		// CAP_PROP_FRAME_COUNT, which is an estimate or 0 for many H.264 and streamed sources) avoids processing empty frames.
		// A live source doesn't run out: readFramePair() reconnects it after a dropout.
		while (readFramePair(FName)){
			if (!runHasFrames){  // the run's first frames: the manifest's "started", and a live run's first manifest
				runHasFrames = true;
				runStarted = frameTime(frameNumber);
				if (liveSource) writeManifest(false);
			}
			// Only frame 1's analysis box in gray is needed: convert it straight from the frame (no copy).
			cv::cvtColor(frame1(AnalysisBox), grayImage1, COLOR_BGR2GRAY);  //convert the box to gray scale for frame differencing
			Mat ROIFr2 = frame2(AnalysisBox).clone();   		   // Carve out the analysis box for motion detection
			cv::cvtColor(ROIFr2, grayImage2, COLOR_BGR2GRAY);   //convert ROIFr2 to gray scale for frame differencing
			cv::absdiff(grayImage1, grayImage2, differenceImage);   			//perform frame differencing
			cv::threshold(differenceImage, thresholdImage, g.SENSITIVITY_VALUE, 255, THRESH_BINARY);  //threshold intensity image at a given sensitivity value
			cv::blur(thresholdImage, thresholdImage, cv::Size(g.BLUR_SIZE, g.BLUR_SIZE));  //blur the image to reduce noise.
			cv::threshold(thresholdImage, thresholdImage, g.SENSITIVITY_VALUE, 255, THRESH_BINARY);	//threshold again to obtain binary image from blur output

			if (showVideo)	imshow("Final Threshold Image", thresholdImage);
			else if (!headless) cv::destroyWindow("Final Threshold Image");

		// ************************************************* Vehicle motion analysis *****************************************************
#ifdef VST_EMBEDDED
			frameBoxes.clear();
#endif
			objectDetected = manageMovers(thresholdImage, ROIFr2);
#ifdef VST_EMBEDDED
			if (hooks && hooks->onFrame) hooks->onFrame({ frame2, thresholdImage, AnalysisBox, frameNumber + 1, frameBoxes });
#endif

			frameNumber += 2;  // Note: frames are used in frame differencing operations only once each, so frame count jumps by two, not one.
			                  // One could argue that using each frame as the second frame in a differencing operation, and then using it a second time
			                  // as the first frame in the next differencing operation would increase resolution.  It probably would.  However,
			                  // doubling frame differencing operations will increase processing times, and probably not add much to speed estimations quality.
			                  // Look at the function computeFinalSpeed() in vehicleDynamics.cpp where I analyze distances of front bumper from the speed zone
			                  // lines to decide whether or not to add or subtract one frame from the total number of frames a vehicle took to pass
			                  // through the speed measuring zone.   I contend performing the +/-1 analysis brings back the accuracy that doubling frame
			                  // differencing operations would provide, but at half the computational cost.
			runFrames += 2;
			runEnded = frameTime(frameNumber);  // the end of the pair just analyzed (frameNumber is now the next frame's number)
			manifestCheckpoint();

			//show captured frame
			 if (showVideo)imshow("Whole Scene", ROIFr2);

			if (!showVideo)
				delay = 1;
			else if (objectDetected) 
				delay = objDelay;
			else 
				delay = 10;

			switch (uiWaitKey(delay)){
			case 27: //'esc'     exit program.
				printSummary();
				dropTracks();
				writeManifest(true);
				return 0;
			case 102: // 'f'    make display go faster;
				if (objDelay > 10) objDelay = objDelay / 5;
				cout << "<" << frameNumber << ">  Delay:" << objDelay << endl;
				break;
			case 112: //'p'     pause/resume.
				pause = !pause;
				if (pause == true){
					cout << "Code paused, press 'p' again to resume" << endl;
					while (pause == true){
						//wait for another p
						switch (waitKey()){
						case 112:
							pause = false;
							cout << "<" << frameNumber << ">  Code Resumed" << endl;
							break;
						} // switch
					} // while paused
				} // if pause
				break;
			case 115: // 's'      slow down display rate
				if (objDelay <1250) objDelay = 5* objDelay;
				cout << "<" << frameNumber << ">  Delay:" << objDelay << endl;
				break;
			case 118:  // 'v'  turn video on/off
				showVideo = !showVideo;
				break;
			} // switch

		} // main loop for processing one input file

		capture->release();
		printSummary();
		// Vehicles still in the zone when the video ended (or the source was stopped) go in the manifest as untimed.  Each
		// video settles the manifest; only the run's last video, or a stop while reconnecting, marks it complete.
		dropTracks();
		bool runOver = fileIndex + 1 == filesToDo.size() || stoppedWhileReconnecting;
		if (runOver) for (Outage& o : outages) if (o.to < 0) o.to = clockNow();  // stopped while down: the gap ran to the stop
		writeManifest(runOver);
		if (stoppedWhileReconnecting) break;

	} // looping over input files loop end

//	if (highLightsPlease) hiLiteVideo.release();
	if (pleaseTrace) traceFile.close();
	if (highLightsPlease) hiLiteVideo.release();
	statsFile.close();
	return 0;

}



#ifndef VST_EMBEDDED
int main(int argc, char** argv){
	return vstMain(argc, argv);
}
#else
namespace {

// Line-buffered stream that forwards each complete line to Hooks::onLog, so the host can show the engine's console.
class LogBuf : public std::streambuf {
public:
	explicit LogBuf(const vst::Hooks& h) : h_(h) {}
	~LogBuf() override { flushLine(); }
protected:
	int overflow(int c) override {
		if (c == traits_type::eof()) return 0;
		if (c == '\n') flushLine(); else line_ += char(c);
		return c;
	}
	std::streamsize xsputn(const char* s, std::streamsize n) override {
		for (std::streamsize i = 0; i < n; i++) overflow((unsigned char)s[i]);
		return n;
	}
private:
	void flushLine(){ if (!line_.empty() && h_.onLog) h_.onLog(line_); line_.clear(); }
	const vst::Hooks& h_;
	std::string line_;
};

// Puts every file-scope global back to the value it has when the CLI process starts.
void resetEngineState(){
	numObjects = 0;
	coalescedRectangle = Rect();
	vehiclesGoingRight.clear();
	vehiclesGoingLeft.clear();
	bailing = false;
	if (traceFile.is_open()) traceFile.close();
	if (statsFile.is_open()) statsFile.close();
	traceFile.clear();
	statsFile.clear();
	capture->release();
	filesToDo.clear();
	fileName.clear();
	objDelay = 50;
	pleaseTrace = false;
	highLightsPlease = false;
	speedLimit = 25;
	egregiousSpeedLowerBound = 35;
	crazySpeed = 55;
	highLightsSpeedLower = 35;
	highLightsSpeedUpper = 100;
	minimumProfileArea = 100;
	frameNumber = 0;
	startFrame = 0.0;
	AnalysisBox = Rect();
	hiLiteVideo.release();
	g.~Globals();           // Globals has a const member, so it can't be assigned; rebuild it in place.
	new (&g) Globals();
	frame1.release();
	frame2.release();
	headless = false;
	inputPath.clear();
	configPath.clear();
	demo = false;
	loggedVehicles = speeders = wrongWayVehicles = 0;
	frameBoxes.clear();
	liveSource = false;
	recordingStart = -1;
	foscamDateTime.clear();
	reconnectAttempts = 0;
	outageStarted = 0;
	stoppedWhileReconnecting = false;
	manifestPath.clear();
	manifestStats.clear();
	manifestInput.clear();
	manifestKind.clear();
	runStarted = runEnded = -1;
	runHasFrames = false;
	runFrames = 0;
	outages.clear();
	lastManifestWrite = chrono::steady_clock::time_point();
	runLogged = runBelowMinSpeed = runUntimed = 0;
}

}  // namespace

int vst::run(const std::vector<std::string>& args, const vst::Hooks& h){
	resetEngineState();
	hooks = &h;
	LogBuf buf(h);
	std::streambuf* oldOut = cout.rdbuf(&buf);
	std::streambuf* oldErr = cerr.rdbuf(&buf);

	std::vector<std::string> storage = { "VideoSpeedTracker", "--headless" };
	storage.insert(storage.end(), args.begin(), args.end());
	std::vector<char*> argv;
	for (auto& a : storage) argv.push_back(a.data());

	int rc;
	try { rc = vstMain(int(argv.size()), argv.data()); }
	catch (const EngineExit& e) { rc = e.code; }
	catch (const std::exception& e) { cerr << "Tracker error: " << e.what() << endl; rc = EXIT_FAILURE; }

	if (traceFile.is_open()) traceFile.close();
	if (statsFile.is_open()) statsFile.close();
	hiLiteVideo.release();
	capture->release();
	cout.flush();
	cout.rdbuf(oldOut);
	cerr.rdbuf(oldErr);
	hooks = nullptr;
	return rc;
}
#endif
