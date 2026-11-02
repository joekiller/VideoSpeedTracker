#include "Calibration.h"
#include "CalEvidence.h"
#include "UserPaths.h"
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace calib {

static const char* kHeader = "Timestamp, Config, Kind, Direction, DrivenMph, Frames, Fps, CalibrationFrames, MeasuredMph, Error";

std::string historyPath() { return (userpaths::configDir() / "calibration-history.csv").string(); }
std::string inputsPath() { return (userpaths::configDir() / "calibration-inputs.json").string(); }

std::string nowIso() {
	// Local fields plus the UTC offset, the same form as the stats csv's Timestamp column.  The offset is the local
	// fields read back as if they were UTC (timegm-style, via the days-from-civil formula) minus the real time;
	// strftime's %z isn't "+hh:mm" and differs on Windows.
	std::time_t t = std::time(nullptr);
	std::tm lt{};
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
	int off = int((days * 86400 + lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec - (long long)t) / 60);
	char s[48];
	snprintf(s, sizeof s, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
		lt.tm_hour, lt.tm_min, lt.tm_sec, off < 0 ? '-' : '+', std::abs(off) / 60, std::abs(off) % 60);
	return s;
}

// Shortest plain form of a number: 25, 29.97.
static std::string num(double v) {
	char s[32];
	snprintf(s, sizeof s, "%g", v);
	return s;
}

static std::string oneDecimal(double v) {
	char s[32];
	snprintf(s, sizeof s, "%.1f", v);
	return s;
}

static std::string canonical(const std::string& p) {
	if (p.empty()) return p;
	std::error_code ec;
	fs::path c = fs::weakly_canonical(fs::path(p), ec);
	return ec ? p : c.string();
}

bool append(const CalEvent& e, std::string& err) {
	std::string path = historyPath();
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	bool fresh = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;
	// Append mode only: earlier rows are evidence and are never rewritten.
	std::ofstream f(path, std::ios::app | std::ios::binary);
	if (!f) { err = "Can't write " + path; return false; }
	if (fresh) f << kHeader << "\n";
	f << e.time << ", " << canonical(e.config) << ", " << (e.check ? "check" : "apply") << ", " << (e.l2r ? "L2R" : "R2L") << ", "
		<< num(e.drivenMph) << ", " << e.frames << ", " << num(e.fps) << ", " << e.calFrames << ", ";
	if (e.check) f << oneDecimal(e.measuredMph) << ", " << oneDecimal(e.error);
	else f << ",";   // MeasuredMph and Error are empty for an apply
	f << "\n";
	f.flush();
	if (!f) { err = "Can't write " + path; return false; }
	return true;
}

static std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string passesPath() { return (userpaths::configDir() / "calibration-passes.csv").string(); }

bool appendPass(const CalPass& p, std::string& err) {
	std::string path = passesPath();
	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	bool fresh = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;
	std::ofstream f(path, std::ios::app | std::ios::binary);
	if (!f) { err = "Can't write " + path; return false; }
	if (fresh) f << "PassTime, Stats, Frame, Kind, Direction, DrivenMph\n";
	f << p.time.substr(0, 19) << ", " << p.stats << ", " << p.frame << ", " << (p.check ? "check" : "apply") << ", " << (p.l2r ? "L2R" : "R2L")
	  << ", " << num(p.drivenMph) << "\n";
	f.flush();
	if (!f) { err = "Can't write " + path; return false; }
	return true;
}

std::vector<CalPass> loadPasses() {
	std::vector<CalPass> out;
	std::ifstream f(passesPath(), std::ios::binary);
	for (std::string line; std::getline(f, line);) {
		std::vector<std::string> raw, parts;
		std::stringstream ss(line);
		for (std::string p; std::getline(ss, p, ',');) { raw.push_back(p); parts.push_back(trim(p)); }
		// Counted from both ends: a video's name (in Stats) may itself hold commas.
		size_t n = parts.size();
		if (n < 6 || (parts[n - 3] != "apply" && parts[n - 3] != "check")) continue;   // the header, or junk
		CalPass p;
		p.time = parts[0];
		for (size_t i = 1; i + 4 < n; i++) p.stats += (i > 1 ? "," : "") + raw[i];
		p.stats = trim(p.stats);
		p.check = parts[n - 3] == "check";
		p.l2r = parts[n - 2] == "L2R";
		try { p.frame = std::stoi(parts[n - 4]); p.drivenMph = std::stod(parts[n - 1]); }
		catch (...) { continue; }
		out.push_back(p);
	}
	return out;
}

std::vector<CalEvent> load(const std::string& configPath) {
	std::vector<CalEvent> out;
	std::ifstream f(historyPath(), std::ios::binary);
	if (!f) return out;
	std::string want = canonical(configPath), line;
	while (std::getline(f, line)) {
		// Fields are ", "-separated and unquoted.  Only the config path can itself hold a comma, so the timestamp is
		// the first field, the last eight are counted from the right, and the path is whatever lies between.
		std::vector<std::string> parts;
		std::stringstream ss(line);
		for (std::string p; std::getline(ss, p, ',');) parts.push_back(p);
		if (!line.empty() && line.back() == ',') parts.push_back("");   // getline drops an empty last field
		if (parts.size() < 10) continue;
		size_t n = parts.size();
		std::string config = parts[1];
		for (size_t i = 2; i + 8 < n; i++) config += "," + parts[i];
		CalEvent e;
		e.time = trim(parts[0]);
		e.config = trim(config);
		std::string kind = trim(parts[n - 8]), dir = trim(parts[n - 7]);
		if ((kind != "apply" && kind != "check") || (dir != "L2R" && dir != "R2L")) continue;   // the header, or junk
		if (canonical(e.config) != want) continue;
		e.check = kind == "check";
		e.l2r = dir == "L2R";
		try {
			e.drivenMph = std::stod(trim(parts[n - 6]));
			e.frames = std::stoi(trim(parts[n - 5]));
			e.fps = std::stod(trim(parts[n - 4]));
			e.calFrames = std::stoi(trim(parts[n - 3]));
			if (e.check) { e.measuredMph = std::stod(trim(parts[n - 2])); e.error = std::stod(trim(parts[n - 1])); }
		}
		catch (...) { continue; }
		out.push_back(e);
	}
	return out;
}

double impliedFrames(double mph, int frames, double fps, double calSpeed, double calFps) {
	if (calSpeed <= 0 || fps <= 0) return 0;
	return frames * mph / calSpeed * calFps / fps;
}

double measuredMph(int calFrames, int frames, double fps, double calSpeed, double calFps) {
	if (frames <= 0 || calFps <= 0) return 0;
	return calSpeed * calFrames * (fps / calFps) / frames;
}

CalState state(const std::vector<CalEvent>& rows, bool l2r, int currentFrames, bool calibratedOffRecord) {
	// The shared rule: a direction is verified when its latest check after its latest apply is within ±2 mph.
	// One addition here: that check must have been made against the CalibrationFrames still in VST.cfg.  If the
	// value was changed some other way since (VST.cfg edited by hand, or PUT /api/config), the check no longer
	// speaks for it, and the direction drops back to "not checked".
	CalState s;
	const CalEvent* apply = nullptr;
	const CalEvent* check = nullptr;
	for (const CalEvent& e : rows) {
		if (e.l2r != l2r) continue;
		if (e.check) check = &e;
		else { apply = &e; check = nullptr; }   // a new apply voids every earlier check
	}
	if (apply) s.applied = apply->time;
	if (check && check->calFrames == currentFrames) {
		s.kind = std::fabs(check->error) <= kVerifiedMph + 1e-9 ? CalState::Verified : CalState::Failed;
		s.error = check->error;
		s.checked = check->time;
	}
	else if (apply || calibratedOffRecord) s.kind = CalState::Unchecked;
	return s;
}

}  // namespace calib
