#include "Results.h"
#include <fstream>
#include <sstream>

static std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static bool toInt(const std::string& s, int& out) {
	try { size_t used; out = std::stoi(s, &used); return trim(s.substr(used)).empty(); }
	catch (...) { return false; }
}

bool readStatsCsv(const std::string& path, std::vector<StatsRow>& rows, std::string& error, int* skipped) {
	std::ifstream in(path);
	if (!in) { error = "Can't open " + path; return false; }
	rows.clear();
	int bad = 0;
	std::string line;
	bool header = true;
	while (std::getline(in, line)) {
		if (header) { header = false; if (line.find("Direction") != std::string::npos) continue; }
		if (trim(line).empty()) continue;
		// Columns: Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed[, *****]
		std::vector<std::string> f;
		std::stringstream ss(line);
		for (std::string cell; std::getline(ss, cell, ',');) f.push_back(trim(cell));
		StatsRow r;
		if (f.size() < 13 || !toInt(f[2], r.frame) || !toInt(f[4], r.startFrame) || !toInt(f[5], r.endFrame) || !toInt(f[6], r.frames)
			|| !toInt(f[7], r.startPix) || !toInt(f[8], r.endPix) || !toInt(f[9], r.deltaPix) || !toInt(f[10], r.area) || !toInt(f[12], r.speed)) {
			bad++;
			continue;
		}
		r.date = f[0];
		r.time = f[1];
		r.compass = f[3];
		r.flagged = f.size() > 13 && f[13].find('*') != std::string::npos;
		rows.push_back(r);
	}
	if (skipped) *skipped = bad;
	return true;
}

bool writeStatsCsv(const std::string& path, const std::vector<StatsRow>& rows) {
	std::ofstream out(path);
	if (!out) return false;
	out << "Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed\n";
	for (const StatsRow& r : rows) {
		out << r.date << ", " << r.time << ", " << r.frame << ", " << r.compass << ", " << r.startFrame << ", " << r.endFrame << ", "
			<< r.frames << ", " << r.startPix << ", " << r.endPix << ", " << r.deltaPix << ", " << r.area << ", , " << r.speed;
		if (r.flagged) out << ", *****";
		out << "\n";
	}
	return bool(out);
}
