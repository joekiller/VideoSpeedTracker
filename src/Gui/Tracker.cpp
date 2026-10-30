#include "Tracker.h"
#include "Media.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static constexpr size_t kRing = 90;          // frame pairs of analysis-box history (~6 s at 30 fps) for snapshots/clips
static constexpr size_t kMaxVehicles = 5000;  // session history cap
static constexpr size_t kMaxLog = 400;

static std::string nowIso() {
	std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	std::ostringstream o;
	o << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S");
	return o.str();
}

static std::string stamp() {
	std::string s = nowIso();
	s.erase(std::remove(s.begin(), s.end(), '-'), s.end());
	s.erase(std::remove(s.begin(), s.end(), ':'), s.end());
	std::replace(s.begin(), s.end(), 'T', '_');
	return s;
}

// Output file stem for a job: the 2016 Foscam naming for manual_yyyymmddhhmmss files, the file stem otherwise,
// and camera<N>_<time> / stream_<time> for live sources so repeated sessions don't overwrite each other.
static std::string jobStem(const std::string& input) {
	switch (classifySource(input)) {
	case SourceInfo::Camera: return "camera" + input + "_" + stamp();
	case SourceInfo::Stream: return "stream_" + stamp();
	default: {
		std::string stem = fs::path(input).stem().string();
		bool foscam = stem.size() == 21 && stem.compare(0, 7, "manual_") == 0;
		return foscam ? stem.substr(7, 8) + "_" + stem.substr(15, 6) : stem;
	}
	}
}

Tracker::~Tracker() {
	stop();
}

void Tracker::start(const std::vector<Job>& jobs, const Options& opt) {
	stop();
	stop_ = false;
	paused_ = false;
	realtime_ = opt.realtime;
	running_ = true;
	{
		std::lock_guard<std::mutex> l(mu_);
		jobs_ = jobs;
		progress_ = Progress();
		progress_.jobs = int(jobs.size());
	}
	thread_ = std::thread(&Tracker::worker, this, jobs, opt);
}

void Tracker::stop() {
	stop_ = true;
	paused_ = false;
	if (thread_.joinable()) thread_.join();
	running_ = false;
}

void Tracker::worker(std::vector<Job> jobs, Options opt) {
	opt_ = opt;
	vst::Hooks hooks;
	hooks.onFrame = [this](const vst::FrameInfo& f) { onFrame(f); };
	hooks.onVehicle = [this](const vst::VehicleEvent& e) { onVehicle(e); };
	hooks.pollKey = [this] { return pollKey(); };
	hooks.onSourceStatus = [this](const vst::SourceStatus& s) { onSourceStatus(s); };
	hooks.onLog = [this](const std::string& line) {
		std::lock_guard<std::mutex> l(mu_);
		log_.push_back(line);
		while (log_.size() > kMaxLog) log_.pop_front();
	};

	fs::path data(opt.dataDir);
	std::error_code ec;
	for (const char* sub : { "Stats", "HiLites", "Trace", "Clips" }) fs::create_directories(data / sub, ec);
	clipsDir_ = (data / "Clips").string();

	for (size_t i = 0; i < jobs.size() && !stop_; i++) {
		Job& job = jobs[i];
		currentInput_ = job.input;
		std::string stem = jobStem(job.input);
		job.statsPath = (data / "Stats" / ("stats_" + stem + ".csv")).string();
		std::vector<std::string> args = {
			"--input", job.input, "--config", opt.configPath, "--stats", job.statsPath,
			"--speed-limit", std::to_string(opt.speedLimit), "--egregious", std::to_string(opt.egregious),
		};
		if (i == 0 && opt.startFrame > 0) { args.push_back("--start-frame"); args.push_back(std::to_string(opt.startFrame)); }
		if (opt.highlights) {
			job.highlightsPath = (data / "HiLites" / ("Hilites_" + stem + ".mp4")).string();
			for (auto& a : { std::string("--highlights"), job.highlightsPath, std::string("--hl-min"), std::to_string(opt.hlMin),
				std::string("--hl-max"), std::to_string(opt.hlMax), std::string("--hl-area"), std::to_string(opt.hlArea) })
				args.push_back(a);
		}
		if (opt.trace) { args.push_back("--trace"); args.push_back((data / "Trace" / ("trace_" + stem + ".txt")).string()); }

		// Probe the source for the progress bar and pacing; the engine opens it again itself.
		{
			Player probe;
			int total = 0;
			fps_ = 30;
			liveSource_ = classifySource(job.input) != SourceInfo::File;
			if (!liveSource_ && probe.open(job.input)) {
				total = probe.info().frames;
				if (probe.info().fps > 0) fps_ = probe.info().fps;
			}
			probe.close();
			std::lock_guard<std::mutex> l(mu_);
			job.state = Job::Running;
			jobs_[i] = job;
			progress_.job = int(i);
			progress_.label = displayName(job.input);
			progress_.frame = opt.startFrame;
			progress_.totalFrames = total;
			progress_.videoFps = fps_;
			progress_.live = liveSource_;
			progress_.reconnecting = false;
			progress_.sourceStatus.clear();
			lastStats_ = job.statsPath;
		}
		ring_.clear();
		paceFrame0_ = -1;
		rateFrames_ = 0;
		rateStart_ = Clock::now();
		int before = lastVehicleId();

		int rc = vst::run(args, hooks);

		std::lock_guard<std::mutex> l(mu_);
		progress_.reconnecting = false;
		job.vehicles = nextId_ - 1 - before;
		job.state = stop_ ? Job::Stopped : rc == 0 ? Job::Done : Job::Failed;
		if (rc != 0 && !log_.empty()) job.message = log_.back();
		jobs_[i] = job;
	}
	running_ = false;
}

int Tracker::pollKey() {
	while (paused_ && !stop_) {
		std::this_thread::sleep_for(std::chrono::milliseconds(15));
		paceFrame0_ = -1;   // restart pacing after a pause instead of racing to catch up
	}
	return stop_ ? 27 : -1;
}

// The engine lost a camera or stream and is reopening it (or has).  Frames from before the gap are dropped from the ring so a
// vehicle tracked afterwards never gets a snapshot or clip from the other side of it.
void Tracker::onSourceStatus(const vst::SourceStatus& s) {
	if (s.reconnecting) ring_.clear();
	std::lock_guard<std::mutex> l(mu_);
	progress_.reconnecting = s.reconnecting;
	progress_.sourceStatus = s.message;
}

void Tracker::onFrame(const vst::FrameInfo& f) {
	box_ = f.analysisBox;
	ring_.push_back({ f.frameNumber, f.frame(f.analysisBox).clone(), f.boxes });
	while (ring_.size() > kRing) ring_.pop_front();

	auto now = Clock::now();
	// Files in real-time mode play at their own rate: frame n is due n/fps seconds after the first.
	if (realtime_ && !liveSource_) {
		if (paceFrame0_ < 0) { paceFrame0_ = f.frameNumber; paceStart_ = now; }
		auto due = paceStart_ + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>((f.frameNumber - paceFrame0_) / fps_));
		if (due > now) std::this_thread::sleep_until(due);
		now = Clock::now();
	}
	else paceFrame0_ = -1;

	rateFrames_ += 2;
	double since = std::chrono::duration<double>(now - rateStart_).count();
	std::lock_guard<std::mutex> l(mu_);
	progress_.frame = f.frameNumber;
	if (since >= .5) {
		progress_.procFps = rateFrames_ / since;
		rateFrames_ = 0;
		rateStart_ = now;
	}
	// Hand the GUI at most ~40 frames a second; copying every frame at 25x real time would be wasted work.
	if (now - lastCopy_ >= std::chrono::milliseconds(24)) {
		lastCopy_ = now;
		f.frame.copyTo(live_.frame);
		f.motion.copyTo(live_.motion);
		live_.analysisBox = f.analysisBox;
		live_.boxes = f.boxes;
		live_.frameNumber = f.frameNumber;
		live_.serial++;
	}
}

void Tracker::onVehicle(const vst::VehicleEvent& e) {
	Vehicle v;
	v.time = nowIso();
	v.video = displayName(currentInput_);
	v.videoPath = currentInput_;
	v.fps = fps_;
	v.live = liveSource_;
	v.ev = e;
	// The engine stamps each vehicle with the time it writes to the csv: the clock for live sources, and for recordings the
	// start time from a Foscam name or the file's modified time, plus the vehicle's offset.  Use the same moment here so the
	// app and the csv agree, without the UTC offset to keep the "yyyy-mm-ddThh:mm:ss" form the rest of the app uses.
	if (!e.timestamp.empty()) {
		if (liveSource_) v.time = e.timestamp.substr(0, 19);
		else v.recorded = e.timestamp.substr(0, 19);
	}
	v.large = e.area >= opt_.largeArea;
	v.speeding = e.speed > opt_.speedLimit;
	v.egregious = e.speed >= opt_.egregious;

	// Snapshot: the ring entry closest to the moment the vehicle crossed the second speed line, cropped to a
	// 2.4:1 window around its tracked box (or its bumper position if no box was drawn that frame).
	const RingEntry* best = nullptr;
	for (const RingEntry& r : ring_)
		if (!best || std::abs(r.frame - e.endFrame) < std::abs(best->frame - e.endFrame)) best = &r;
	auto cropFor = [&](const RingEntry& r) {
		int h = r.roi.rows, w = std::min(r.roi.cols, int(h * 2.4));
		int cx = e.endPixel + (e.leftToRight ? -w / 4 : w / 4);
		int bestDist = 1 << 30;
		for (const vst::TrackBox& b : r.boxes)
			if (b.leftToRight == e.leftToRight) {
				int front = e.leftToRight ? b.box.x + b.box.width : b.box.x;
				if (std::abs(front - e.endPixel) < bestDist) { bestDist = std::abs(front - e.endPixel); cx = b.box.x + b.box.width / 2; }
			}
		int x = std::clamp(cx - w / 2, 0, r.roi.cols - w);
		return cv::Rect(x, 0, w, h);
	};
	if (best) v.snapshot = best->roi(cropFor(*best)).clone();

	// Clip: the analysis box from a little before the first speed line to now, for the companion app.
	std::vector<const RingEntry*> clip;
	for (const RingEntry& r : ring_)
		if (r.frame >= e.startFrame - 20) clip.push_back(&r);
	if (clip.size() >= 4) {
		int id;
		{ std::lock_guard<std::mutex> l(mu_); id = nextId_; }
		std::string path = (fs::path(clipsDir_) / ("vehicle_" + stamp() + "_" + std::to_string(id) + ".mp4")).string();
		cv::Size size(clip[0]->roi.cols & ~1, clip[0]->roi.rows & ~1);
		cv::VideoWriter w;
		// H.264 plays in browsers; fall back to MPEG-4 Part 2 where this OpenCV/FFmpeg has no H.264 encoder.
		for (int fourcc : { cv::VideoWriter::fourcc('a', 'v', 'c', '1'), cv::VideoWriter::fourcc('m', 'p', '4', 'v') }) {
			if (w.open(path, fourcc, fps_ / 2, size, true)) break;
		}
		if (w.isOpened()) {
			for (const RingEntry* r : clip) w.write(r->roi(cv::Rect(0, 0, size.width, size.height)));
			w.release();
			v.clipPath = path;
		}
	}

	std::lock_guard<std::mutex> l(mu_);
	v.id = nextId_++;
	vehicles_.push_back(std::move(v));
	while (vehicles_.size() > kMaxVehicles) vehicles_.pop_front();
}

bool Tracker::latest(Live& out, bool wantMotion) {
	std::lock_guard<std::mutex> l(mu_);
	if (live_.serial == out.serial || live_.frame.empty()) return false;
	live_.frame.copyTo(out.frame);
	if (wantMotion) live_.motion.copyTo(out.motion);
	out.analysisBox = live_.analysisBox;
	out.boxes = live_.boxes;
	out.frameNumber = live_.frameNumber;
	out.serial = live_.serial;
	return true;
}

cv::Mat Tracker::latestFrame() {
	std::lock_guard<std::mutex> l(mu_);
	return live_.frame.clone();
}

Tracker::Progress Tracker::progress() {
	std::lock_guard<std::mutex> l(mu_);
	return progress_;
}

std::vector<Job> Tracker::jobs() {
	std::lock_guard<std::mutex> l(mu_);
	return jobs_;
}

std::vector<Vehicle> Tracker::vehiclesSince(int afterId, size_t limit) {
	std::lock_guard<std::mutex> l(mu_);
	std::vector<Vehicle> out;
	for (const Vehicle& v : vehicles_)
		if (v.id > afterId) out.push_back(v);
	if (limit && out.size() > limit) out.erase(out.begin(), out.end() - limit);
	return out;
}

bool Tracker::vehicle(int id, Vehicle& out) {
	std::lock_guard<std::mutex> l(mu_);
	for (const Vehicle& v : vehicles_)
		if (v.id == id) { out = v; return true; }
	return false;
}

int Tracker::lastVehicleId() {
	std::lock_guard<std::mutex> l(mu_);
	return nextId_ - 1;
}

Counts Tracker::counts() {
	std::lock_guard<std::mutex> l(mu_);
	Counts c;
	double sum = 0;
	for (const Vehicle& v : vehicles_) {
		c.vehicles++;
		c.speeders += v.speeding;
		c.egregious += v.egregious;
		c.wrongWay += v.ev.wrongWay;
		c.topSpeed = std::max(c.topSpeed, v.ev.speed);
		sum += v.ev.speed;
	}
	c.avgSpeed = c.vehicles ? sum / c.vehicles : 0;
	return c;
}

void Tracker::clearVehicles() {
	std::lock_guard<std::mutex> l(mu_);
	vehicles_.clear();
}

std::vector<std::string> Tracker::logTail(size_t n) {
	std::lock_guard<std::mutex> l(mu_);
	size_t from = log_.size() > n ? log_.size() - n : 0;
	return std::vector<std::string>(log_.begin() + from, log_.end());
}

std::string Tracker::lastStatsPath() {
	std::lock_guard<std::mutex> l(mu_);
	return lastStats_;
}
