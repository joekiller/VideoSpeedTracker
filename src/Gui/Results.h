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
	// The speed trend columns (v1.10+; src/Wintel64/VideoSpeedTracker/SpeedTrend.h).  trend is "decelerating", "steady",
	// "accelerating" or "rapid", or "" when the tracker couldn't tell or the csv predates the columns; the numbers are
	// meaningful only when it is set.
	std::string trend;
	int entryMph = 0, exitMph = 0;  // at the two speed lines
	double accel = 0;              // mph per second, + speeding up
	int mphIn20ft = 0;             // projected 20 ft past the exit line
	int lengthPx = 0;              // LengthPx (v1.10+): side-on length in pixels, 0 when unknown or the csv predates it
	double zoneSeconds = 0;        // ZoneSeconds (v1.10+): seconds between the speed lines, 0 when unknown (CalSets.h re-times with it)
};

// Reads a stats csv.  Returns false with error set if the file can't be read; malformed rows are skipped and counted.
bool readStatsCsv(const std::string& path, std::vector<StatsRow>& rows, std::string& error, int* skipped = nullptr);
// Writes rows in the tracker's current format, e.g. for "export filtered".
bool writeStatsCsv(const std::string& path, const std::vector<StatsRow>& rows);
