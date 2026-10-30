// Stats csv files the tracker writes (one row per vehicle), read back for the Results page.
#pragma once
#include <string>
#include <vector>

struct StatsRow {
	std::string date, time;        // "yyyymmdd", "hhmmss"; for Foscam file names, when the recording started; may be blank
	int frame = 0;                 // frame number when the vehicle was logged
	std::string compass;           // L2RDirection / R2LDirection label, e.g. "NE"
	int startFrame = 0, endFrame = 0, frames = 0;
	int startPix = 0, endPix = 0, deltaPix = 0;
	int area = 0;
	int speed = 0;
	bool flagged = false;          // "*****": implausibly fast, worth checking by eye
	std::string timestamp;         // when it passed, ISO 8601 with UTC offset; empty if unknown or the csv predates the column
};

// Reads a stats csv.  Returns false with error set if the file can't be read; malformed rows are skipped and counted.
bool readStatsCsv(const std::string& path, std::vector<StatsRow>& rows, std::string& error, int* skipped = nullptr);
// Writes rows in the tracker's current format, e.g. for "export filtered".
bool writeStatsCsv(const std::string& path, const std::vector<StatsRow>& rows);
