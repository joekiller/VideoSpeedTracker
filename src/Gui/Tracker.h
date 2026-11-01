// Runs the embedded tracker (vst::run) on a worker thread, one job (video file, camera or stream) after another,
// and publishes what the GUI and the companion API need: the latest frame with its vehicle boxes, each logged
// vehicle with a snapshot and a short clip, counts, progress and the engine's log.
//
// Flow per job: build CLI-style arguments -> vst::run() with hooks -> onFrame copies the frame (rate-limited when
// running faster than real time) and keeps a short ring of analysis-box crops -> onVehicle cuts the snapshot and
// clip for that vehicle out of the ring -> pollKey implements pause/stop and real-time pacing -> onSourceStatus tracks a
// live source that dropped out and is being reconnected by the engine.
#pragma once
#include "Engine.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

struct Job {
	enum State { Queued, Running, Done, Failed, Stopped };
	std::string input;          // path, camera index or URL
	State state = Queued;
	int vehicles = 0;
	std::string statsPath, highlightsPath, message;
	std::string manifestPath;   // the engine's run manifest beside statsPath (vst::manifestPathFor), once the job has started
};

struct Vehicle {
	int id = 0;
	std::string time;           // ISO 8601, local wall clock when logged
	std::string video;          // display name of the source
	std::string videoPath;
	double fps = 30;
	bool live = false;          // from a camera or stream (time is when it passed) rather than a recording
	std::string recorded;       // ISO time it passed in a recording, when the engine knows when the recording started
	vst::VehicleEvent ev;
	bool large = false;
	bool speeding = false, egregious = false;
	int frames() const { return std::max(ev.endFrame - ev.startFrame, 1); }
	cv::Mat snapshot;           // BGR crop around the vehicle at the end speed line
	std::string clipPath;       // short mp4 of the analysis box, empty if none
};

// Session totals.  vehicles are the ones logged (timed, a clean track, at least minLoggedSpeed: a csv row each); the rest
// of what the tracker tried to time is slow (timed but under minLoggedSpeed) and untimed (followed into the speed zone
// but never given a usable speed).  tries = vehicles + slow + untimed, every car that crossed into the zone, so a
// view where cars are seen but not timed shows as tries climbing while vehicles doesn't (vst::AttemptEvent).
struct Counts {
	int vehicles = 0, speeders = 0, egregious = 0, wrongWay = 0, topSpeed = 0;
	double avgSpeed = 0;
	int slow = 0, untimed = 0, tries = 0;
};

class Tracker {
public:
	struct Options {
		std::string configPath;     // VST.cfg the run uses
		std::string dataDir;        // Stats/, HiLites/, Clips/, Trace/ go under here
		int speedLimit = 25, egregious = 35;
		bool realtime = true;       // files play at their own frame rate instead of as fast as possible
		bool highlights = true;
		int hlMin = 35, hlMax = 100, hlArea = 100;
		bool trace = false;
		int startFrame = 0;
		int largeArea = 109;        // largeVehicleArea from the config, to flag trucks and buses
	};
	~Tracker();
	void start(const std::vector<Job>& jobs, const Options& opt);
	void stop();
	void setPaused(bool p) { paused_ = p; }
	bool paused() const { return paused_; }
	bool running() const { return running_; }
	void setRealtime(bool r) { realtime_ = r; }

	// Latest frame for display; returns false if nothing new since the last call with this serial.
	struct Live {
		cv::Mat frame, motion;
		cv::Rect analysisBox;
		std::vector<vst::TrackBox> boxes;
		int frameNumber = 0;
		long serial = 0;
	};
	bool latest(Live& out, bool wantMotion);
	cv::Mat latestFrame();          // copy of the newest frame (for the API), empty if none

	struct Progress {
		int job = -1, jobs = 0;
		std::string label;
		int frame = 0, totalFrames = 0;
		double videoFps = 0, procFps = 0;
		bool live = false;          // camera or stream
		bool reconnecting = false;  // a live source dropped out and the engine is reopening it
		std::string sourceStatus;   // the engine's latest reconnect line, e.g. "Reconnecting to camera 0: attempt 3 in 4 s"
	};
	Progress progress();
	std::vector<Job> jobs();

	// Vehicles are kept for the whole session (newest last), across runs, until clear().
	std::vector<Vehicle> vehiclesSince(int afterId, size_t limit = 0);
	bool vehicle(int id, Vehicle& out);
	int lastVehicleId();
	Counts counts();
	void clearVehicles();
	std::vector<std::string> logTail(size_t n);
	std::string lastStatsPath();

private:
	void worker(std::vector<Job> jobs, Options opt);
	void onFrame(const vst::FrameInfo& f);
	void onVehicle(const vst::VehicleEvent& e);
	void onSourceStatus(const vst::SourceStatus& s);
	int pollKey();

	std::thread thread_;
	std::atomic<bool> running_{ false }, stop_{ false }, paused_{ false }, realtime_{ true };
	std::mutex mu_;
	Live live_;
	std::deque<Vehicle> vehicles_;
	int slow_ = 0, untimed_ = 0;   // tried but not logged this session (Counts)
	int nextId_ = 1;
	std::vector<Job> jobs_;
	Progress progress_;
	std::deque<std::string> log_;
	std::string lastStats_;

	// Worker-thread only state.
	struct RingEntry { int frame; cv::Mat roi; std::vector<vst::TrackBox> boxes; };
	std::deque<RingEntry> ring_;
	Options opt_;
	std::string currentInput_, clipsDir_;
	double fps_ = 30;
	bool liveSource_ = false;
	std::chrono::steady_clock::time_point paceStart_, lastCopy_, rateStart_;
	int paceFrame0_ = -1, rateFrames_ = 0;
	cv::Rect box_;
};
