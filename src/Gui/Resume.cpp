// Picking up where the user left off when VST Studio reopens (see Resume.h for the rules).
#include "Resume.h"
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace resume {

bool reopenAtLaunch(const std::string& lastSource, bool isPath, bool exists) {
	// A camera or stream has no file to check: try it, in the background.  A file that has gone stays closed (its
	// name is still under Recent), rather than greeting the user with "can't open" for a video they moved.
	return !lastSource.empty() && (!isPath || exists);
}

std::string setupSource(const std::string& connected, const std::string& remembered) {
	return connected.empty() ? remembered : connected;
}

bool sourceStepDone(bool connected, bool queued, const std::string& remembered) {
	return connected || queued || !remembered.empty();
}

bool zonesStepDone(const std::string& setupSource, const std::string& zonesFor, bool configOk) {
	return !setupSource.empty() && zonesFor == setupSource && configOk;
}

std::vector<SavedRun> savedRuns(const std::string& dataDir) {
	std::vector<SavedRun> runs;
	if (dataDir.empty()) return runs;
	std::error_code ec;
	fs::path dir = fs::path(dataDir) / "Stats";
	for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
		const fs::path& p = it->path();
		std::string file = p.filename().string();
		if (!it->is_regular_file(ec) || p.extension() != ".csv" || file.rfind("stats_", 0) != 0) continue;
		SavedRun r;
		r.csv = p.string();
		r.name = p.stem().string().substr(6);
		std::error_code tec;
		auto t = fs::last_write_time(p, tec);
		r.modified = tec ? 0 : std::int64_t(t.time_since_epoch().count());
		runs.push_back(r);
	}
	// Newest first.  Runs made within the same clock tick (or with no time) fall back to the name, whose trailing
	// yyyymmdd_hhmmss stamp sorts the same way.
	std::sort(runs.begin(), runs.end(), [](const SavedRun& a, const SavedRun& b) {
		return a.modified != b.modified ? a.modified > b.modified : a.name > b.name;
	});
	return runs;
}

static bool isDigit(char c) { return c >= '0' && c <= '9'; }

// "20261101_133012" -> "2026-11-01 13:30"; anything else is returned unchanged.
static std::string stampText(const std::string& s) {
	if (s.size() != 15 || s[8] != '_' || !std::all_of(s.begin(), s.begin() + 8, isDigit) || !std::all_of(s.begin() + 9, s.end(), isDigit)) return s;
	return s.substr(0, 4) + "-" + s.substr(4, 2) + "-" + s.substr(6, 2) + " " + s.substr(9, 2) + ":" + s.substr(11, 2);
}

std::string runLabel(const std::string& name) {
	// Tracker.cpp's jobStem: camera<index>_<stamp>, picam<index>_<stamp>, stream_<stamp>, or the video's own stem.
	size_t us = name.find('_');
	if (us == std::string::npos || name.size() < us + 16) return name;
	std::string head = name.substr(0, us), stamp = name.substr(us + 1);
	if (stampText(stamp) == stamp) return name;
	auto digitsAfter = [&](const char* prefix) {
		size_t n = std::char_traits<char>::length(prefix);
		return head.size() > n && head.compare(0, n, prefix) == 0 && std::all_of(head.begin() + n, head.end(), isDigit);
	};
	if (digitsAfter("picam")) return "Pi camera " + head.substr(5) + "  ·  " + stampText(stamp);
	if (digitsAfter("camera")) return "Camera " + head.substr(6) + "  ·  " + stampText(stamp);
	if (head == "picam") return "Pi camera  ·  " + stampText(stamp);
	if (head == "stream") return "Stream  ·  " + stampText(stamp);
	return name;
}

}  // namespace resume
