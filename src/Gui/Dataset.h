// Published datasets: a locked calibration set (CalSets.h) frozen into a folder with a documented, versioned schema
// (docs/dataset-schema.md, "vst-dataset/1"), checksums, and the accuracy it promises, that anyone can open in VST
// Studio's reviewer or in any browser (review.html), and that can be re-checked when the same view is calibrated again.
//
// <data>/Published/<YYYY-MM-DD>_<name>/ is an archive folder (Archive.h: Stats/, archive.json, review.html, VST.cfg,
// calibration-history.csv, Snapshots/, Clips/) plus:
//
//   dataset.json        what it is: schema, id, name, site, limits, period, the calibration (zone feet and its 95% band per
//                       direction) and the headline numbers.  Never changed after publishing.
//   points.csv          one row per car: the documented columns (Point), nothing internal.  Never changed.
//   SHA256SUMS          checksums of every file above (not review.html, status.json or checks/), `sha256sum -c` format
//   status.json         appended to: one entry per check against a later calibration, with its verdict
//   checks/<when>.csv   for each check, every point's speed before and after and the difference
//
// Re-checking (check()): a calibration only rescales speeds, so a later calibration of the same view gives every car a new
// speed, zone feet / ZoneSeconds.  Whether the published numbers still hold is a question about the calibration itself:
// delta = new zone feet / published zone feet - 1, against the band the dataset promised, T = 1.96 x its relative sigma.
// Two one-sided tests at 95% (TOST), with the new calibration's own sigma:
//
//   Still accurate   |delta| + 1.645 sigma_new <= T     the change is surely inside the promise
//   Tainted          |delta| - 1.645 sigma_new >  T     the change is surely outside it: speeds are off by more than promised
//   Under review     otherwise                          can't tell yet: another check drive narrows sigma_new
//
// The worst direction decides.  A tainted dataset is never edited or hidden: its badge says so and a corrected copy can be
// published beside it (correctionOf).  No ImGui: tests/dataset_test.cpp drives it.
#pragma once
#include "CalSets.h"
#include <string>
#include <vector>

namespace dataset {

constexpr const char* kSchema = "vst-dataset/1";

enum class Verdict { None, Accurate, Review, Tainted };
const char* verdictName(Verdict v);    // "", "accurate", "review", "tainted" (the json value)
const char* verdictLabel(Verdict v);   // "Not re-checked", "Still accurate", "Under review", "Tainted: needs correction"
Verdict verdictFrom(const std::string& name);

// One car, as points.csv documents it.
struct Point {
	int n = 0;                     // row number from 1
	std::string passed;            // ISO 8601 local time ("" = unknown)
	bool l2r = true;
	std::string compass;
	bool wrongWay = false;
	double zoneSeconds = 0;        // seconds between the speed lines, 0 = not recorded (speed was then rescaled)
	int speed = 0;                 // mph at the dataset's calibration
	std::string trend;             // decelerating | steady | accelerating | rapid | ""
	int entryMph = 0, exitMph = 0, mphIn20ft = 0;
	double accel = 0;
	double lengthFt = 0;
	bool flagged = false;          // implausible, left out of the numbers
	int ownDrive = 0;              // 1 calibration drive, 2 check drive: the person calibrating, left out of the numbers
	std::string run;               // the stats csv it came from (in Stats/)
	int frame = 0;                 // its Frame there: run + frame finds the raw row
};

// The calibration a dataset's speeds are at.  Index 0 = L2R, 1 = R2L.
struct Calibration {
	double zoneFt[2] = { 0, 0 };
	double sigma[2] = { 0, 0 };    // relative 1-sigma (CalEvidence), 0 = unknown
	std::string how;               // plain words: "3 measurements agree within 2%"
	double band(int dir) const;    // the promised 95% band, relative: 1.96 x sigma (0.06 assumed when unknown)
};

// The headline numbers: traffic only (no own drives, nothing flagged).
struct Summary {
	int cars = 0, over = 0, wayOver = 0, wrongWay = 0;
	int p85 = 0;                   // 85th percentile, mph
};
Summary summarize(const std::vector<Point>& pts, int speedLimit, int wayOver);

struct Test {
	double delta = 0;              // new / published - 1
	double sigmaNew = 0, tolerance = 0;
	double lo = 0, hi = 0;         // 90% interval of delta (the TOST interval)
	Verdict verdict = Verdict::None;
};
Test equivalence(double publishedFt, double publishedSigma, double newFt, double newSigma);

struct Check {
	std::string when;              // ISO 8601
	Verdict verdict = Verdict::None;
	Calibration cal;               // the newer calibration
	Test test[2];                  // per direction; a direction with no cars has verdict None
	Summary before, after;
	double meanDiff = 0, maxDiff = 0;   // mph, after - before, over traffic (maxDiff = largest by size, signed)
	std::string file;              // checks/<...>.csv
};

struct Info {
	std::string dir;               // the folder (filled in by read())
	std::string schema = kSchema;
	std::string id;                // the folder's name, e.g. "2026-11-01_locust-week-1"
	std::string name, site, published, vst;
	int version = 1;
	std::string correctionOf;      // id of the dataset this corrects ("" = none)
	int speedLimit = 25, wayOver = 35;
	std::string l2rCompass = "SE", r2lCompass = "NW", legal = "Both";
	std::string first, last;       // earliest and latest passed
	int calibrationSet = 0;        // the set it came from (CalSets.h)
	calsets::View view;            // the view it was measured from: a later calibration is comparable only on the same view
	Calibration cal;
	Summary summary;
	std::vector<Point> points;
	std::vector<Check> checks;     // from status.json, oldest first
	Verdict status() const { return checks.empty() ? Verdict::None : checks.back().verdict; }
};

// Files as written.  datasetJson has no checks; coverJson = dataset.json plus "checks" and "status", for review.html.
std::string datasetJson(const Info& in);
std::string pointsCsv(const Info& in);
std::string statusJson(const Info& in);
std::string coverJson(const Info& in);

// Reads dataset.json, points.csv and status.json.  A newer major schema is refused with a plain error.
bool read(const std::string& dir, Info& out, std::string& error);
bool isDataset(const std::string& dir);
std::vector<Info> list(const std::string& dataDir);   // every readable <dataDir>/Published/* , newest first

// Re-hashes the files SHA256SUMS lists: changed gets each one that differs or is missing.  false = no SHA256SUMS.
bool verify(const std::string& dir, std::vector<std::string>& changed);

// What a later calibration would do to every point, without writing anything.
Check evaluate(const Info& in, const Calibration& newer, const std::string& now, std::vector<double>* after = nullptr);
// evaluate() and record it: checks/<when>.csv, status.json, and review.html redrawn with the new badge.
bool check(Info& in, const Calibration& newer, const std::string& now, Check& out, std::string& error);

// Plain words for the cover.
std::string accuracyWords(const Calibration& c, int speedLimit);   // "within ±1 mph at 25 mph (±3%)"
std::string sentence(const Info& in);                              // the one sentence under the badge

}  // namespace dataset
