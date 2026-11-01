#include "Results.h"
#include <cstdio>
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
		// Columns: Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed,
		// Flag, Timestamp.  Csvs written before Flag and Timestamp existed end at estSpeed, with a 14th "*****" only when flagged,
		// so both formats are read the same way: column 13 is the flag if present, column 14 the time if present.  Columns 15-19
		// (v1.10+) are the speed trend: EntryMph, ExitMph, AccelMphPerSec, Trend, MphIn20ft, all empty when unknown.
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
		if (f.size() > 14) r.timestamp = f[14];
		if (f.size() > 19 && !f[18].empty()) {
			try {
				r.entryMph = std::stoi(f[15]);
				r.exitMph = std::stoi(f[16]);
				r.accel = std::stod(f[17]);
				r.mphIn20ft = std::stoi(f[19]);
				r.trend = f[18];
			}
			catch (...) { r.trend.clear(); }   // a hand-edited row: keep the vehicle, drop its trend
		}
		rows.push_back(r);
	}
	if (skipped) *skipped = bad;
	return true;
}

bool writeStatsCsv(const std::string& path, const std::vector<StatsRow>& rows) {
	std::ofstream out(path);
	if (!out) return false;
	out << "Date, Time, Frame, Direction, StartFrame, EndFrame, # Frames, StartPix, EndPix, DeltaPix, VehicleArea, , estSpeed, Flag, Timestamp, "
		"EntryMph, ExitMph, AccelMphPerSec, Trend, MphIn20ft\n";
	for (const StatsRow& r : rows) {
		out << r.date << ", " << r.time << ", " << r.frame << ", " << r.compass << ", " << r.startFrame << ", " << r.endFrame << ", "
			<< r.frames << ", " << r.startPix << ", " << r.endPix << ", " << r.deltaPix << ", " << r.area << ", , " << r.speed << ", "
			<< (r.flagged ? "*****" : "") << ", " << r.timestamp << ", ";
		if (r.trend.empty()) out << ", , , , \n";
		else {
			char a[32];
			snprintf(a, sizeof a, "%.1f", r.accel);
			out << r.entryMph << ", " << r.exitMph << ", " << a << ", " << r.trend << ", " << r.mphIn20ft << "\n";
		}
	}
	return bool(out);
}
