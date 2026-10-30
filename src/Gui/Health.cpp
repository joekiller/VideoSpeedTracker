#include "Health.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <opencv2/imgproc.hpp>

namespace health {

namespace {

struct Entry {
	fs::path path;
	std::uintmax_t size;
	fs::file_time_type mtime;
};

std::vector<Entry> listFiles(const std::vector<fs::path>& dirs) {
	std::vector<Entry> out;
	for (const fs::path& d : dirs) {
		std::error_code ec;
		for (fs::directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
			std::error_code e2;
			if (!it->is_regular_file(e2)) continue;
			std::uintmax_t size = it->file_size(e2);
			if (e2) continue;
			fs::file_time_type t = it->last_write_time(e2);
			if (e2) continue;
			out.push_back({ it->path(), size, t });
		}
	}
	return out;
}

// Edge strength of a frame at 320x180: the scene's fixed structure (curbs, poles, house fronts) carries the
// correlation, and gradients change much less than raw brightness between noon and dusk.
cv::Mat edges(const cv::Mat& in) {
	cv::Mat g, f, gx, gy, mag;
	if (in.channels() == 3) cv::cvtColor(in, g, cv::COLOR_BGR2GRAY);
	else if (in.channels() == 4) cv::cvtColor(in, g, cv::COLOR_BGRA2GRAY);
	else g = in;
	cv::resize(g, g, cv::Size(1280, 720), 0, 0, cv::INTER_AREA);   // the tracker's frame space first, like vst::run
	cv::resize(g, g, cv::Size(320, 180), 0, 0, cv::INTER_AREA);
	g.convertTo(f, CV_32F, 1.0 / 255);
	cv::GaussianBlur(f, f, cv::Size(3, 3), 0);
	cv::Sobel(f, gx, CV_32F, 1, 0);
	cv::Sobel(f, gy, CV_32F, 0, 1);
	cv::magnitude(gx, gy, mag);
	// Per-image contrast normalisation, so a dim evening frame weighs the same as a bright one.
	cv::Scalar mean, sd;
	cv::meanStdDev(mag, mean, sd);
	if (sd[0] > 1e-6) mag = (mag - mean[0]) / sd[0];
	return mag;
}

}  // namespace

Usage usage(const std::vector<fs::path>& dirs) {
	Usage u;
	for (const Entry& e : listFiles(dirs)) { u.bytes += e.size; u.files++; }
	return u;
}

std::uintmax_t freeBytes(const fs::path& path) {
	std::error_code ec;
	fs::space_info s = fs::space(path, ec);
	return ec ? 0 : s.available;
}

std::uintmax_t targetBytes(const Usage& used, std::uintmax_t budget, std::uintmax_t freeNow, std::uintmax_t minFree) {
	std::uintmax_t target = budget == 0 ? used.bytes : std::min(budget, used.bytes);
	if (freeNow > 0 && freeNow < minFree) {
		std::uintmax_t need = minFree - freeNow;
		target = target > need ? target - need : 0;
	}
	return target;
}

Pruned pruneTo(const std::vector<fs::path>& dirs, std::uintmax_t target, std::chrono::seconds protect) {
	Pruned p;
	std::vector<Entry> files = listFiles(dirs);
	std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.mtime < b.mtime; });
	std::uintmax_t total = 0;
	for (const Entry& e : files) total += e.size;
	const fs::file_time_type keepAfter = fs::file_time_type::clock::now() - protect;
	for (const Entry& e : files) {
		if (total <= target) break;
		if (e.mtime > keepAfter) break;   // sorted: everything from here on is newer
		std::error_code ec;
		if (!fs::remove(e.path, ec) || ec) continue;
		total -= e.size;
		p.freed += e.size;
		p.removed.push_back(e.path.string());
	}
	p.left = usage(dirs);
	return p;
}

ViewCheck compareView(const cv::Mat& reference, const cv::Mat& current) {
	ViewCheck v;
	if (reference.empty() || current.empty()) return v;
	cv::Mat a = edges(reference), b = edges(current), window;
	cv::createHanningWindow(window, a.size(), CV_32F);
	double response = 0;
	cv::Point2d shift = cv::phaseCorrelate(a, b, window, &response);
	// 320x180 back to 1280x720 pixels.
	v.dx = shift.x * 4;
	v.dy = shift.y * 4;
	v.confidence = std::isfinite(response) ? response : 0;
	if (v.confidence < kMinConfidence) v.result = ViewCheck::Unclear;
	else v.result = std::hypot(v.dx, v.dy) > kMovedPx ? ViewCheck::Moved : ViewCheck::Same;
	return v;
}

const char* csvHeader() {
	return "Date, TrackedHours, OutageHours, Outages, Vehicles, ClipsRemoved, FreeGB, Camera";
}

std::string csvRow(const Day& d) {
	char buf[256];
	char free[32] = "";
	if (d.freeGB >= 0) snprintf(free, sizeof free, "%.1f", d.freeGB);
	snprintf(buf, sizeof buf, "%s, %.2f, %.2f, %d, %d, %d, %s, %s", d.date.c_str(), d.trackedHours, d.outageHours, d.outages,
		d.vehicles, d.clipsRemoved, free, d.camera.c_str());
	return buf;
}

bool appendDay(const fs::path& path, const Day& d) {
	std::error_code ec;
	bool fresh = !fs::exists(path, ec) || fs::file_size(path, ec) == 0;
	if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
	std::ofstream out(path, std::ios::app | std::ios::binary);
	if (!out) return false;
	if (fresh) out << csvHeader() << "\n";
	out << csvRow(d) << "\n";
	return bool(out);
}

}  // namespace health
