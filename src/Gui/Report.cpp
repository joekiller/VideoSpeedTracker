#include "Report.h"
#include "Results.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace report {

// ======================================================================================================
// Small helpers: numbers, dates, text
// ======================================================================================================

int percentile(const std::vector<int>& sorted, double p) {
	if (sorted.empty()) return 0;
	// Nearest rank: the smallest value with at least p% of the list at or below it.  The epsilon keeps 0.85 * 20 = 17
	// from becoming rank 18 through floating-point error.
	size_t rank = size_t(std::ceil(p / 100.0 * double(sorted.size()) - 1e-9));
	rank = std::clamp<size_t>(rank, 1, sorted.size());
	return sorted[rank - 1];
}

static std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::string fmt(const char* f, double v) {
	char b[64];
	snprintf(b, sizeof b, f, v);
	return b;
}

// Percent with one decimal under 10 (so "2.4%" isn't rounded to "2%"), whole numbers above.
static std::string pct(double v) { return v > 0 && v < 9.95 ? fmt("%.1f%%", v) : fmt("%.0f%%", v); }

static std::string esc(const std::string& s) {
	std::string o;
	for (char c : s) {
		switch (c) {
		case '&': o += "&amp;"; break;
		case '<': o += "&lt;"; break;
		case '>': o += "&gt;"; break;
		case '"': o += "&quot;"; break;
		case '\'': o += "&#39;"; break;
		default: o += c;
		}
	}
	return o;
}

// Howard Hinnant's days_from_civil / civil_from_days: proleptic Gregorian calendar, days since 1970-01-01.
static int64_t daysFromCivil(int y, int m, int d) {
	y -= m <= 2;
	int64_t era = (y >= 0 ? y : y - 399) / 400;
	int64_t yoe = y - era * 400;
	int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + doe - 719468;
}

static void civilFromDays(int64_t z, int& y, int& m, int& d) {
	z += 719468;
	int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	int64_t doe = z - era * 146097;
	int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	int64_t mp = (5 * doy + 2) / 153;
	d = int(doy - (153 * mp + 2) / 5 + 1);
	m = int(mp < 10 ? mp + 3 : mp - 9);
	y = int(yoe + era * 400 + (m <= 2));
}

static int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }
static int64_t dayOf(int64_t t) { return floorDiv(t, 86400); }
static int hourOf(int64_t t) { return int((t - dayOf(t) * 86400) / 3600); }
static int weekdayOf(int64_t day) { return int(((day % 7) + 11) % 7); }   // 1970-01-01 was a Thursday (4); 0 = Sunday

bool parseIso(const std::string& in, int64_t& local, int& offset, bool& hasOffset) {
	std::string s = trim(in);
	int Y, M, D, h = 0, m = 0, sec = 0, n = 0;
	if (sscanf(s.c_str(), "%4d-%2d-%2d%n", &Y, &M, &D, &n) != 3 || n != 10) return false;
	size_t p = 10;
	if (p < s.size() && (s[p] == 'T' || s[p] == ' ')) {
		int used = 0;
		if (sscanf(s.c_str() + p + 1, "%2d:%2d:%2d%n", &h, &m, &sec, &used) < 2) return false;
		if (used == 0) used = 5;   // "hh:mm" only
		p += 1 + used;
	}
	if (M < 1 || M > 12 || D < 1 || D > 31 || h > 23 || m > 59 || sec > 60) return false;
	local = daysFromCivil(Y, M, D) * 86400 + h * 3600 + m * 60 + sec;
	hasOffset = false;
	offset = 0;
	while (p < s.size() && s[p] == '.') { p++; while (p < s.size() && isdigit((unsigned char)s[p])) p++; }   // fractions
	if (p < s.size() && (s[p] == 'Z' || s[p] == 'z')) hasOffset = true;
	else if (p < s.size() && (s[p] == '+' || s[p] == '-')) {
		int oh = 0, om = 0;
		if (sscanf(s.c_str() + p + 1, "%2d:%2d", &oh, &om) < 1 && sscanf(s.c_str() + p + 1, "%2d%2d", &oh, &om) < 1) return false;
		offset = (s[p] == '-' ? -1 : 1) * (oh * 3600 + om * 60);
		hasOffset = true;
	}
	return true;
}

bool parseDay(const std::string& s, int64_t& day) {
	int64_t t;
	int off;
	bool has;
	if (trim(s).size() != 10 || !parseIso(s, t, off, has)) return false;
	day = dayOf(t);
	return true;
}

std::string dayString(int64_t day) {
	int y, m, d;
	civilFromDays(day, y, m, d);
	char b[16];
	snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
	return b;
}

static const char* const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char* const kWeekdays[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

// "30 Jan 2016"
static std::string niceDay(int64_t day) {
	int y, m, d;
	civilFromDays(day, y, m, d);
	return std::to_string(d) + " " + kMonths[m - 1] + " " + std::to_string(y);
}

// "30 Jan – 2 Feb 2016", "30 Jan 2016" for one day.
static std::string niceRange(int64_t a, int64_t b) {
	if (a == b) return niceDay(a);
	int y1, m1, d1, y2, m2, d2;
	civilFromDays(a, y1, m1, d1);
	civilFromDays(b, y2, m2, d2);
	std::string left = std::to_string(d1) + " " + kMonths[m1 - 1] + (y1 != y2 ? " " + std::to_string(y1) : "");
	return left + " – " + niceDay(b);
}

// "Sat 30 Jan 17:22"
static std::string niceTime(int64_t t) {
	int64_t day = dayOf(t);
	int y, m, d;
	civilFromDays(day, y, m, d);
	int64_t s = t - day * 86400;
	char b[48];
	snprintf(b, sizeof b, "%.3s %d %s %02d:%02d", kWeekdays[weekdayOf(day)], d, kMonths[m - 1], int(s / 3600), int(s / 60 % 60));
	return b;
}

static std::string hourLabel(int h) { return h == 0 ? "12a" : h < 12 ? std::to_string(h) + "a" : h == 12 ? "12p" : std::to_string(h - 12) + "p"; }

std::string slug(const std::string& s) {
	std::string o;
	for (unsigned char c : s) {
		if (std::isalnum(c)) o += char(std::tolower(c));
		else if (!o.empty() && o.back() != '-') o += '-';
	}
	while (!o.empty() && o.back() == '-') o.pop_back();
	if (o.size() > 48) o.resize(48);
	return o.empty() ? "site" : o;
}

static std::string base64(const std::string& in) {
	static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string o;
	o.reserve((in.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 2 < in.size(); i += 3) {
		unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
		o += t[v >> 18]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += t[v & 63];
	}
	if (i + 1 == in.size()) {
		unsigned v = (unsigned char)in[i] << 16;
		o += t[v >> 18]; o += t[v >> 12 & 63]; o += "==";
	}
	else if (i + 2 == in.size()) {
		unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
		o += t[v >> 18]; o += t[v >> 12 & 63]; o += t[v >> 6 & 63]; o += '=';
	}
	return o;
}

std::string RunConfig::resolveLegal() const {
	// LegalDirection may be written as the compass label of one direction (e.g. "NE"), as VST.cfg allows.
	if (legal == "L2R" || (!l2rCompass.empty() && legal == l2rCompass)) return "L2R";
	if (legal == "R2L" || (!r2lCompass.empty() && legal == r2lCompass)) return "R2L";
	return "Both";
}

// ======================================================================================================
// Loading: stats csvs, manifests, calibration history
// ======================================================================================================

// stats_<stem>.csv -> run_<stem>.json; any other name gets run_ prefixed (the --stats PATH rule in the contract).
static fs::path manifestFor(const fs::path& csv) {
	std::string stem = csv.stem().string();
	std::string name = stem.rfind("stats_", 0) == 0 ? "run_" + stem.substr(6) : "run_" + stem;
	return csv.parent_path() / (name + ".json");
}

static void readManifest(const fs::path& p, Run& run) {
	std::ifstream in(p);
	if (!in) return;
	json j = json::parse(in, nullptr, false, true);   // allow comments, as in the contract's example
	if (j.is_discarded() || !j.is_object()) { run.problem = p.filename().string() + " couldn't be read"; return; }
	run.manifest = true;
	auto str = [](const json& o, const char* k) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); };
	auto num = [](const json& o, const char* k, double d) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : d; };
	run.vst = str(j, "vst");
	if (j.contains("source") && j["source"].is_object()) run.sourceKind = str(j["source"], "kind");
	run.complete = !j.contains("complete") || !j["complete"].is_boolean() || j["complete"].get<bool>();
	run.fps = num(j, "fps", 0);
	int64_t a, b;
	int off;
	bool has;
	if (parseIso(str(j, "started"), a, off, has) && parseIso(str(j, "ended"), b, off, has) && b >= a) {
		run.timed = true;
		run.span = { a, b };
	}
	if (j.contains("outages") && j["outages"].is_array())
		for (const json& o : j["outages"])
			if (o.is_object() && parseIso(str(o, "from"), a, off, has) && parseIso(str(o, "to"), b, off, has) && b > a) run.outages.push_back({ a, b });
	if (j.contains("config") && j["config"].is_object()) {
		const json& c = j["config"];
		RunConfig& rc = run.cfg;
		rc.calSpeed = num(c, "CalibrationSpeed", rc.calSpeed);
		rc.calFps = num(c, "CalibrationFPS", rc.calFps);
		rc.framesL2R = int(num(c, "CalibrationFramesL2R", rc.framesL2R));
		rc.framesR2L = int(num(c, "CalibrationFramesR2L", rc.framesR2L));
		if (!str(c, "LegalDirection").empty()) rc.legal = str(c, "LegalDirection");
		if (!str(c, "L2RDirection").empty()) rc.l2rCompass = str(c, "L2RDirection");
		if (!str(c, "R2LDirection").empty()) rc.r2lCompass = str(c, "R2LDirection");
		rc.minLoggedSpeed = int(num(c, "minLoggedSpeed", rc.minLoggedSpeed));
		run.cfgFromManifest = true;
	}
	if (j.contains("counts") && j["counts"].is_object()) {
		const json& c = j["counts"];
		run.logged = int(num(c, "logged", -1));
		run.belowMin = int(num(c, "belowMinSpeed", -1));
		run.untimed = int(num(c, "untimed", -1));
	}
}

// Times for a row.  The Timestamp column (VST 1.1+) is when the vehicle passed.  Older csvs only have Date/Time, which
// for Foscam recordings (manual_yyyymmddhhmmss) is when the recording started, the same on every row: the vehicle's
// offset into the video (Frame / fps) is added.  For other sources Date/Time was already the moment it passed.
static int64_t rowTime(const StatsRow& r, bool recordingStart, double fps) {
	int64_t t;
	int off;
	bool has;
	if (!r.timestamp.empty() && parseIso(r.timestamp, t, off, has)) return t;
	if (r.date.size() != 8 || r.time.size() != 6) return kNoTime;
	std::string iso = r.date.substr(0, 4) + "-" + r.date.substr(4, 2) + "-" + r.date.substr(6, 2) + "T" + r.time.substr(0, 2) + ":" + r.time.substr(2, 2) + ":" + r.time.substr(4, 2);
	if (!parseIso(iso, t, off, has)) return kNoTime;
	if (recordingStart && fps > 0) t += int64_t(r.frame / fps);
	return t;
}

std::vector<Run> loadRuns(const Input& in, std::vector<Obs>& obs) {
	std::vector<fs::path> files;
	for (const std::string& f : in.csvFiles) files.push_back(f);
	if (in.csvFiles.empty()) {
		std::error_code ec;
		fs::path dir = fs::path(in.dataDir) / "Stats";
		for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
			if (it->is_regular_file(ec) && it->path().extension() == ".csv") files.push_back(it->path());
		std::sort(files.begin(), files.end());
	}
	std::vector<Run> runs;
	for (const fs::path& f : files) {
		Run run;
		run.csvPath = f.string();
		run.name = f.filename().string();
		run.cfg = in.current;
		readManifest(manifestFor(f), run);
		std::vector<StatsRow> rows;
		std::string err;
		if (!readStatsCsv(f.string(), rows, err, &run.skipped)) { run.problem = err; runs.push_back(run); continue; }
		// A csv whose header has no Timestamp column predates it (13 columns); read the header line to tell.
		{
			std::ifstream h(f);
			std::string line;
			std::getline(h, line);
			run.columns = line.find("Timestamp") != std::string::npos ? 15 : 13;
		}
		// Foscam-style Date/Time (recording start) is the same on every row; per-vehicle times differ.
		bool sameStart = !rows.empty();
		for (const StatsRow& r : rows) sameStart &= r.date == rows[0].date && r.time == rows[0].time;
		bool recordingStart = sameStart && (rows.size() > 1 || run.name.find("manual_") != std::string::npos);
		double fps = run.fps > 0 ? run.fps : run.cfg.calFps;
		std::string legal = run.cfg.resolveLegal();
		int idx = int(runs.size());
		for (const StatsRow& r : rows) {
			Obs o;
			o.run = idx;
			o.t = rowTime(r, recordingStart, fps);
			if (o.t == kNoTime) run.undated++;
			else if (r.timestamp.empty()) run.timesDerived = true;
			// The csv names directions by compass label; anything else falls back to which way the bumper moved.
			o.l2r = r.compass == run.cfg.l2rCompass || (r.compass != run.cfg.r2lCompass && r.endPix > r.startPix);
			o.speed = r.speed;
			o.flagged = r.flagged;
			o.wrongWay = (o.l2r && legal == "R2L") || (!o.l2r && legal == "L2R");
			obs.push_back(o);
		}
		runs.push_back(run);
	}
	return runs;
}

static std::vector<std::string> splitCsv(const std::string& line) {
	std::vector<std::string> f;
	std::stringstream ss(line);
	for (std::string cell; std::getline(ss, cell, ',');) f.push_back(trim(cell));
	if (!line.empty() && line.back() == ',') f.push_back("");
	return f;
}

static bool samePath(const std::string& a, const std::string& b) {
	if (a == b) return true;
	std::error_code e1, e2;
	fs::path ca = fs::weakly_canonical(a, e1), cb = fs::weakly_canonical(b, e2);
	return !e1 && !e2 && ca == cb;
}

static void readHistory(const Input& in, Study& st) {
	std::set<std::string> seen;
	for (const std::string& file : in.historyFiles) {
		std::error_code ec;
		std::string key = fs::weakly_canonical(file, ec).string();
		if (file.empty() || !seen.insert(key).second) continue;
		std::ifstream f(file);
		if (!f) continue;
		st.historyFound = true;
		std::string line;
		while (std::getline(f, line)) {
			if (trim(line).empty() || line.rfind("Timestamp", 0) == 0) continue;
			std::vector<std::string> c = splitCsv(line);
			if (c.size() < 10) continue;
			// A config path may itself hold commas: the first column is the time, the last eight are fixed, and
			// whatever lies between is the path.
			std::string cfg;
			for (size_t i = 1; i + 8 < c.size(); i++) cfg += (i > 1 ? "," : "") + c[i];
			const std::string* t = &c[c.size() - 8];
			if (!in.configPath.empty() && !samePath(cfg, in.configPath)) { st.historyOtherConfigs++; continue; }
			CalEvent e;
			e.when = c[0];
			e.kind = t[0];
			e.dir = t[1];
			try {
				e.driven = std::stod(t[2]);
				e.frames = std::stoi(t[3]);
				e.fps = std::stod(t[4]);
				e.calFrames = std::stoi(t[5]);
				if (!t[6].empty()) { e.measured = std::stod(t[6]); e.hasMeasured = true; }
				if (!t[7].empty()) e.error = std::stod(t[7]);
				else if (e.hasMeasured) e.error = e.measured - e.driven;
			}
			catch (...) { continue; }
			if ((e.kind != "apply" && e.kind != "check") || (e.dir != "L2R" && e.dir != "R2L")) continue;
			st.calEvents.push_back(e);
		}
	}
	// Verified: the latest check after the latest apply in that direction is within 2 mph.  A check with no apply in the
	// history (calibrated before the history existed) still counts: it tested the calibration in force.
	for (int d = 0; d < 2; d++) {
		const char* dir = d == 0 ? "L2R" : "R2L";
		int lastApply = -1, lastCheck = -1;
		for (int i = 0; i < int(st.calEvents.size()); i++) {
			if (st.calEvents[i].dir != dir) continue;
			if (st.calEvents[i].kind == "apply") { lastApply = i; lastCheck = -1; }
			else lastCheck = i;
		}
		if (lastCheck >= 0) {
			st.hasCheck[d] = true;
			st.checkError[d] = st.calEvents[lastCheck].error;
			st.checkCalFrames[d] = st.calEvents[lastCheck].calFrames;
			st.verified[d] = std::fabs(st.calEvents[lastCheck].error) <= 2.0;
		}
	}
}

// ======================================================================================================
// Analysis
// ======================================================================================================

static SpeedStats speedStats(std::vector<int> v, const Settings& s) {
	SpeedStats r;
	r.n = int(v.size());
	if (v.empty()) return r;
	std::sort(v.begin(), v.end());
	double sum = 0;
	for (int x : v) {
		sum += x;
		r.over += x > s.speedLimit;               // "over the limit" = strictly above it, as everywhere in VST
		r.over10 += x >= s.speedLimit + 10;
		r.wayOver += x >= s.wayOver;
	}
	r.mean = sum / r.n;
	r.median = percentile(v, 50);
	r.p85 = percentile(v, 85);
	r.max = v.back();
	r.pctOver = 100.0 * r.over / r.n;
	r.pct10 = 100.0 * r.over10 / r.n;
	return r;
}

// Sorts and merges overlapping intervals.
static std::vector<Interval> mergeIntervals(std::vector<Interval> v) {
	std::sort(v.begin(), v.end(), [](const Interval& a, const Interval& b) { return a.from < b.from; });
	std::vector<Interval> out;
	for (const Interval& i : v) {
		if (i.to <= i.from) continue;
		if (!out.empty() && i.from <= out.back().to) out.back().to = std::max(out.back().to, i.to);
		else out.push_back(i);
	}
	return out;
}

// a minus every interval in cut.
static std::vector<Interval> subtract(Interval a, std::vector<Interval> cut) {
	cut = mergeIntervals(cut);
	std::vector<Interval> out;
	int64_t at = a.from;
	for (const Interval& c : cut) {
		if (c.to <= at || c.from >= a.to) continue;
		if (c.from > at) out.push_back({ at, c.from });
		at = std::max(at, c.to);
	}
	if (at < a.to) out.push_back({ at, a.to });
	return out;
}

Study analyze(const Input& in, const Settings& s) {
	Study st;
	st.settings = s;
	std::vector<Obs> all;
	st.runs = loadRuns(in, all);

	// ---- the date range: as given, or from the first to the last thing with a date ----
	int64_t lo = INT64_MAX, hi = INT64_MIN;
	for (const Obs& o : all) if (o.t != kNoTime) { lo = std::min(lo, dayOf(o.t)); hi = std::max(hi, dayOf(o.t)); }
	for (const Run& r : st.runs) if (r.timed) { lo = std::min(lo, dayOf(r.span.from)); hi = std::max(hi, dayOf(r.span.to)); }
	bool anyDated = lo != INT64_MAX;
	int64_t d;
	st.fromDay = parseDay(s.from, d) ? d : anyDated ? lo : 0;
	st.toDay = parseDay(s.to, d) ? d : anyDated ? hi : -1;
	if (st.toDay < st.fromDay && (!s.from.empty() || !s.to.empty()) && anyDated) std::swap(st.fromDay, st.toDay);
	bool rangeGiven = !s.from.empty() || !s.to.empty();
	if (anyDated || rangeGiven) { st.from = dayString(st.fromDay); st.to = dayString(st.toDay); }
	const int64_t r0 = st.fromDay * 86400, r1 = (st.toDay + 1) * 86400;

	// ---- vehicles in range ----
	std::vector<char> runUsed(st.runs.size(), 0);
	for (const Obs& o : all) {
		if (o.t == kNoTime) {
			// Undated rows can't be placed in a date range; they count only in an all-data report.
			if (rangeGiven) { st.undatedExcluded++; continue; }
			st.undatedIncluded++;
		}
		else if (o.t < r0 || o.t >= r1) continue;
		runUsed[o.run] = 1;
		if (o.flagged) { st.flaggedExcluded++; continue; }
		st.obs.push_back(o);
	}

	// ---- coverage: manifest started..ended minus outages, else first..last vehicle of that csv ----
	std::vector<Interval> spans;
	for (size_t i = 0; i < st.runs.size(); i++) {
		Run& r = st.runs[i];
		Interval span;
		if (r.timed) span = r.span;
		else {
			int64_t a = INT64_MAX, b = INT64_MIN;
			for (const Obs& o : all) if (o.run == int(i) && o.t != kNoTime) { a = std::min(a, o.t); b = std::max(b, o.t); }
			if (a == INT64_MAX) continue;
			span = { a, b };
		}
		Interval clipped{ std::max(span.from, r0), std::min(span.to, r1) };
		if (clipped.to <= clipped.from && !runUsed[i]) continue;
		runUsed[i] = 1;
		if (!r.timed) st.coverageEstimated = true;
		for (const Interval& piece : subtract(clipped, r.outages)) spans.push_back(piece);
	}
	st.covered = mergeIntervals(spans);
	for (const Interval& c : st.covered) {
		st.hours += (c.to - c.from) / 3600.0;
		// Walk the interval an hour bucket at a time, crediting each local day and hour with the seconds observed in it.
		for (int64_t t = c.from; t < c.to;) {
			int64_t next = std::min(c.to, (floorDiv(t, 3600) + 1) * 3600);
			auto& row = st.dayHourSecs[dayOf(t)];
			row[hourOf(t)] += double(next - t);
			st.hourSecs[hourOf(t)] += double(next - t);
			t = next;
		}
	}
	// Gaps of an hour or more between the first and last observation.
	for (size_t i = 1; i < st.covered.size(); i++)
		if (st.covered[i].from - st.covered[i - 1].to >= 3600) st.gaps.push_back({ st.covered[i - 1].to, st.covered[i].from });

	// ---- volumes: whole-run manifest counts for the runs that contributed ----
	for (size_t i = 0; i < st.runs.size(); i++) {
		if (!runUsed[i]) continue;
		const Run& r = st.runs[i];
		if (r.manifest && r.logged >= 0) {
			st.manifestRuns++;
			st.mLogged += r.logged;
			st.mBelow += std::max(0, r.belowMin);
			st.mUntimed += std::max(0, r.untimed);
			st.minLoggedSpeed = r.cfg.minLoggedSpeed;
		}
		else st.plainRuns++;
	}
	if (!st.manifestRuns) st.minLoggedSpeed = in.current.minLoggedSpeed;

	// ---- speeds ----
	std::vector<int> vAll, vL, vR;
	std::array<std::vector<int>, 24> byHour;
	std::set<int> weekdays;
	for (const Obs& o : st.obs) {
		vAll.push_back(o.speed);
		(o.l2r ? vL : vR).push_back(o.speed);
		if (o.t == kNoTime) continue;
		int h = hourOf(o.t), wd = weekdayOf(dayOf(o.t));
		st.hourN[h]++;
		byHour[h].push_back(o.speed);
		st.wdN[wd][h]++;
		st.wdOver[wd][h] += o.speed > s.speedLimit;
		weekdays.insert(wd);
	}
	st.weekdays = int(weekdays.size());
	st.all = speedStats(vAll, s);
	st.l2r = speedStats(vL, s);
	st.r2l = speedStats(vR, s);
	for (int h = 0; h < 24; h++) {
		std::sort(byHour[h].begin(), byHour[h].end());
		st.hourP85[h] = int(byHour[h].size()) >= kMinCell ? percentile(byHour[h], 85) : -1;
	}
	if (!vAll.empty()) {
		auto mm = std::minmax_element(vAll.begin(), vAll.end());
		for (int mph = *mm.first; mph <= *mm.second; mph++) st.histogram.push_back({ mph, 0 });
		for (int v : vAll) st.histogram[v - *mm.first].second++;
	}

	// ---- direction labels and the one-way rule: from the newest run with a manifest, else the current config ----
	RunConfig dirCfg = in.current;
	for (size_t i = 0; i < st.runs.size(); i++)
		if (runUsed[i] && st.runs[i].cfgFromManifest) dirCfg = st.runs[i].cfg;
	st.l2rCompass = dirCfg.l2rCompass;
	st.r2lCompass = dirCfg.r2lCompass;
	st.legal = dirCfg.resolveLegal();
	std::set<std::string> legals;
	for (size_t i = 0; i < st.runs.size(); i++) if (runUsed[i]) legals.insert(st.runs[i].cfg.resolveLegal());
	if (st.legal != "Both")
		for (const Obs& o : st.obs) {
			if (!o.wrongWay) continue;
			st.wrongTotal++;
			if (o.t != kNoTime) st.wrongByHour[hourOf(o.t)]++;
		}

	// ---- calibration record and the error band ----
	readHistory(in, st);
	// Frames a car at the limit spends between the lines: CalibrationFrames scaled to the video's frame rate and from
	// CalibrationSpeed to the limit.  Missing the true crossing by one frame (the tracker sees whole frames) changes the
	// speed by about limit / (N - 1).  Fewer frames = coarser, so the worst run and direction sets the band.
	double nMin[2] = { 1e9, 1e9 };
	bool anyRun = false;
	for (size_t i = 0; i < st.runs.size(); i++) {
		if (!runUsed[i]) continue;
		const Run& r = st.runs[i];
		double fps = r.fps > 0 ? r.fps : r.cfg.calFps;
		double scale = fps / r.cfg.calFps * r.cfg.calSpeed / std::max(1, s.speedLimit);
		nMin[0] = std::min(nMin[0], r.cfg.framesL2R * scale);
		nMin[1] = std::min(nMin[1], r.cfg.framesR2L * scale);
		anyRun = true;
	}
	if (!anyRun) {
		const RunConfig& c = in.current;
		double scale = c.calSpeed / std::max(1, s.speedLimit);
		nMin[0] = c.framesL2R * scale;
		nMin[1] = c.framesR2L * scale;
	}
	// A check only vouches for the calibration it tested.  If a run used a different CalibrationFrames (from its
	// manifest, or the current VST.cfg for runs without one) than the latest check's, the calibration was changed
	// without a history row (a hand edit of VST.cfg): that direction is "not checked" and the check's error is dropped.
	for (int k = 0; k < 2; k++) {
		st.usedCalFrames[k] = k == 0 ? in.current.framesL2R : in.current.framesR2L;
		bool first = true;
		for (size_t i = 0; i < st.runs.size(); i++) {
			if (!runUsed[i]) continue;
			int f = k == 0 ? st.runs[i].cfg.framesL2R : st.runs[i].cfg.framesR2L;
			if (first || (st.hasCheck[k] && f != st.checkCalFrames[k])) st.usedCalFrames[k] = f;
			first = false;
			if (st.hasCheck[k] && f != st.checkCalFrames[k]) break;
		}
		if (st.hasCheck[k] && st.usedCalFrames[k] != st.checkCalFrames[k]) {
			st.checkMismatch[k] = true;
			st.verified[k] = false;
		}
	}
	double worst = 0;
	for (int k = 0; k < 2; k++) {
		st.framesAtLimit[k] = nMin[k];
		double q = s.speedLimit / std::max(1.5, nMin[k] - 1);
		double c = st.hasCheck[k] && !st.checkMismatch[k] ? std::fabs(st.checkError[k]) : 0;
		st.quantMph = std::max(st.quantMph, q);
		st.calMph = std::max(st.calMph, c);
		worst = std::max(worst, q + c);
	}
	// + 0.5 because the csv rounds every speed to a whole mph.  Rounded up to the next tenth, never down.
	st.band = std::ceil((worst + 0.5) * 10 - 1e-9) / 10;

	// ---- caveats: say plainly what is missing or estimated ----
	int used = 0, noManifest = 0, old = 0, derived = 0, incomplete = 0;
	std::set<double> derivedFps;
	for (size_t i = 0; i < st.runs.size(); i++) {
		if (!runUsed[i]) continue;
		const Run& r = st.runs[i];
		used++;
		noManifest += !r.manifest;
		old += r.columns == 13;
		if (r.timesDerived) { derived++; derivedFps.insert(r.fps > 0 ? r.fps : r.cfg.calFps); }
		incomplete += r.manifest && !r.complete;
		if (!r.problem.empty()) st.caveats.push_back(r.name + ": " + r.problem + ".");
	}
	// "1 run has", "2 runs have".
	auto count = [](int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); };
	if (noManifest)
		st.caveats.push_back(std::to_string(noManifest) + " of " + std::to_string(used) + (noManifest == 1 ? " runs has" : " runs have") + " no run manifest (recorded before VST 1.2). "
			"Their observed time is estimated from the first to the last vehicle logged, outages in them are unknown, and "
			"vehicles below the logging minimum or that couldn't be timed weren't counted.");
	if (old) st.caveats.push_back(count(old, "stats csv predates", "stats csvs predate") + " the Timestamp column (13 columns).");
	if (derived) {
		std::string f;
		for (double x : derivedFps) f += (f.empty() ? "" : ", ") + fmt("%g", x);
		st.caveats.push_back("Times were rebuilt in " + count(derived, "csv", "csvs") + " from the Date and Time columns "
			"(the recording's start) plus each vehicle's frame number at " + f + " fps.");
	}
	if (incomplete) st.caveats.push_back(count(incomplete, "run was", "runs were") + " still in progress when this report was made.");
	if (st.undatedExcluded) st.caveats.push_back(count(st.undatedExcluded, "vehicle has", "vehicles have") + " no time at all and " + (st.undatedExcluded == 1 ? "is" : "are") + " left out of this date range.");
	if (st.undatedIncluded) st.caveats.push_back(count(st.undatedIncluded, "vehicle has", "vehicles have") + " no time: counted in the speed statistics but not in the time-of-day charts or coverage.");
	if (st.flaggedExcluded) st.caveats.push_back(count(st.flaggedExcluded, "vehicle", "vehicles") + " flagged as implausible (*****) left out of every statistic.");
	if (legals.size() > 1) st.caveats.push_back("The one-way setting (LegalDirection) changed between runs; the wrong-way count uses each run's own setting.");
	for (int k = 0; k < 2; k++)
		if (st.checkMismatch[k])
			st.caveats.push_back(std::string("The ") + (k == 0 ? st.l2rCompass : st.r2lCompass) + "-bound calibration used ("
				+ std::to_string(st.usedCalFrames[k]) + " frames) is not the one last checked (" + std::to_string(st.checkCalFrames[k])
				+ " frames), so that direction counts as not checked.");
	if (!st.historyFound) st.caveats.push_back("No calibration history was found (VST before 1.2 didn't keep one), so the calibration can't be shown to have been checked.");
	if (s.includeSnapshots && in.snapshots.empty()) st.caveats.push_back("Example frames were requested, but none are available: snapshots exist only for vehicles tracked in the current VST Studio session.");

	if (s.includeSnapshots) st.snapshots = in.snapshots;

	// ---- the before/after comparison: the same analysis on the second range ----
	auto summary = [](const Study& x) {
		PeriodSummary p;
		p.from = x.from;
		p.to = x.to;
		p.hours = x.hours;
		p.all = x.all;
		p.wrongWay = x.wrongTotal;
		p.perHour = x.hours > 0 ? (x.all.n + x.flaggedExcluded) / x.hours : 0;
		return p;
	};
	st.main = summary(st);
	if (!s.compareFrom.empty() || !s.compareTo.empty()) {
		Settings s2 = s;
		s2.from = s.compareFrom;
		s2.to = s.compareTo;
		s2.compareFrom.clear();
		s2.compareTo.clear();
		s2.includeSnapshots = false;
		Study other = analyze(in, s2);
		st.compare = summary(other);
		st.hasCompare = true;
	}
	return st;
}

// ======================================================================================================
// Rendering
// ======================================================================================================

namespace {

// Report palette: ink and hairlines for the document, one accent, and three speed bands for the charts (neutral at or
// under the limit, amber over it, red at 10+ over), each always paired with a legend or label.
const char* kCss = R"CSS(
@page { size: auto; margin: 14mm 14mm 15mm; }
:root {
  --ink: #15181d; --ink2: #464b55; --muted: #767b85; --rule: #c9ccd2; --hair: #e3e5e9; --tint: #f4f5f7;
  --accent: #1d3a5f; --ok: #9aa8b8; --over: #e0a12a; --bad: #c0392b; --cover: #3d6a9e;
}
* { box-sizing: border-box; }
html { background: #e9ebee; }
body { margin: 0; color: var(--ink); background: #e9ebee; font: 9.5pt/1.45 "Helvetica Neue", Helvetica, Arial, system-ui, sans-serif;
  -webkit-print-color-adjust: exact; print-color-adjust: exact; }
.sheet { width: 210mm; min-height: 270mm; margin: 10mm auto; padding: 16mm 15mm 14mm; background: #fff; box-shadow: 0 1px 4px rgba(0,0,0,.12); position: relative; }
@media print {
  html, body { background: #fff; }
  .sheet { width: auto; min-height: 0; margin: 0; padding: 0; box-shadow: none; break-after: page; page-break-after: always; }
  .sheet:last-child { break-after: auto; page-break-after: auto; }
  figure, table, .keep { break-inside: avoid; page-break-inside: avoid; }
}
/* On a phone (the companion opens the report there): one column of sheets, two-up figures, wide tables scroll. */
@media screen and (max-width: 820px) {
  .sheet { width: auto; margin: 0 0 8px; padding: 20px 16px; min-height: 0; }
  /* .sheet-prefixed so these win over the base rules further down */
  .sheet.cover h1 { font-size: 20pt; }
  .sheet .meta, .sheet .kpis { grid-template-columns: 1fr 1fr; row-gap: 4mm; }
  .sheet .kpi:nth-child(3) { padding-left: 0; border-left: 0; }
  .sheet .status { flex-direction: column; gap: 1mm; }
  .sheet table { display: block; overflow-x: auto; }
  .sheet .two, .sheet .snaps { grid-template-columns: 1fr; }
}
h1, h2, h3 { font-weight: 600; margin: 0; letter-spacing: -.01em; }
h2 { font-size: 15pt; color: var(--accent); padding-bottom: 2mm; border-bottom: 1.5pt solid var(--accent); margin-bottom: 4mm; }
h2 .num { color: var(--muted); font-weight: 400; margin-right: 2.5mm; }
h3 { font-size: 10.5pt; margin: 6mm 0 2mm; }
p { margin: 0 0 2.5mm; }
.lede { color: var(--ink2); font-size: 10pt; max-width: 165mm; }
.small { font-size: 8pt; color: var(--muted); }
.num, td.n, th.n, .tab { font-variant-numeric: tabular-nums; }
.runhead { display: flex; justify-content: space-between; font-size: 7.5pt; color: var(--muted); text-transform: uppercase; letter-spacing: .08em; margin-bottom: 6mm; }
.foot { margin-top: 6mm; padding-top: 2mm; border-top: .5pt solid var(--hair); font-size: 7.5pt; color: var(--muted); display: flex; justify-content: space-between; }

/* cover */
.kicker { font-size: 8pt; letter-spacing: .14em; text-transform: uppercase; color: var(--accent); font-weight: 600; }
.cover h1 { font-size: 26pt; line-height: 1.12; margin: 3mm 0 1.5mm; }
.cover .sub { font-size: 12pt; color: var(--ink2); }
.label { display: inline-block; margin: 5mm 0 6mm; padding: 2mm 3.5mm; border: 1pt solid var(--accent); color: var(--accent); font-weight: 600; font-size: 9.5pt; }
.meta { display: grid; grid-template-columns: repeat(4, 1fr); border-top: .75pt solid var(--rule); border-bottom: .75pt solid var(--rule); }
.meta div { padding: 2.5mm 3mm 2.5mm 0; }
.meta dt { font-size: 7.5pt; text-transform: uppercase; letter-spacing: .08em; color: var(--muted); }
.meta dd { margin: .5mm 0 0; font-weight: 600; }
.kpis { display: grid; grid-template-columns: 1.3fr 1fr 1fr 1fr; gap: 0; margin: 7mm 0 5mm; }
.kpi { padding: 0 4mm; border-left: .75pt solid var(--hair); }
.kpi:first-child { padding-left: 0; border-left: 0; }
.kpi b { display: block; font-size: 24pt; font-weight: 600; line-height: 1.05; letter-spacing: -.02em; }
.kpi:first-child b { font-size: 34pt; color: var(--accent); }
.kpi b small { font-size: 11pt; font-weight: 400; color: var(--ink2); margin-left: 1mm; letter-spacing: 0; }
.kpi span { display: block; margin-top: 1.5mm; font-size: 8.5pt; color: var(--ink2); line-height: 1.3; }
.findings { margin: 0 0 4mm; padding-left: 4.5mm; }
.findings li { margin-bottom: 1.5mm; }
.status { display: flex; gap: 3mm; align-items: baseline; padding: 2.5mm 3.5mm; background: var(--tint); border-left: 2pt solid var(--accent); margin: 3mm 0; }
.status.warn { border-left-color: var(--bad); }
.status b { white-space: nowrap; }
.note { margin-top: 4mm; padding: 3mm 3.5mm; border: .75pt solid var(--rule); }
.note h3 { margin: 0 0 1mm; font-size: 8pt; text-transform: uppercase; letter-spacing: .08em; color: var(--muted); }

/* tables */
table { width: 100%; border-collapse: collapse; margin: 1mm 0 3mm; }
th { font-size: 7.5pt; font-weight: 600; text-transform: uppercase; letter-spacing: .06em; color: var(--muted); text-align: left; padding: 1.5mm 2mm; border-bottom: .75pt solid var(--ink); }
td { padding: 1.4mm 2mm; border-bottom: .5pt solid var(--hair); vertical-align: top; }
th.n, td.n { text-align: right; }
tr.total td { font-weight: 600; border-top: .75pt solid var(--ink); border-bottom: 0; }
td.muted { color: var(--muted); }
.two { display: grid; grid-template-columns: 1fr 1fr; gap: 7mm; }

/* figures */
figure { margin: 2mm 0 5mm; }
figcaption { font-size: 8.5pt; color: var(--ink2); margin-top: 1mm; }
figcaption b { color: var(--ink); }
svg { display: block; width: 100%; height: auto; overflow: visible; }
svg text { font-family: "Helvetica Neue", Helvetica, Arial, system-ui, sans-serif; fill: var(--muted); font-size: 9px; font-variant-numeric: tabular-nums; }
svg .ink { fill: var(--ink); }
svg .ink2 { fill: var(--ink2); }
svg .grid { stroke: var(--hair); stroke-width: .75; }
svg .axis { stroke: var(--rule); stroke-width: 1; }
svg .limit { stroke: var(--ink); stroke-width: 1.2; stroke-dasharray: 4 3; }
.legend { display: flex; flex-wrap: wrap; gap: 4mm; font-size: 8pt; color: var(--ink2); margin: 1mm 0 0; }
.legend i { display: inline-block; width: 3mm; height: 3mm; border-radius: .6mm; margin-right: 1.2mm; vertical-align: -.4mm; }
.legend i.line { height: 0; border-top: 1.2pt dashed var(--ink); border-radius: 0; vertical-align: .6mm; width: 5mm; }
.snaps { display: grid; grid-template-columns: 1fr 1fr; gap: 4mm; }
.snaps img { width: 100%; display: block; border: .5pt solid var(--rule); }
.snaps figcaption { font-variant-numeric: tabular-nums; }
.method h3 { margin-top: 5mm; }
.method p, .method li { max-width: 172mm; }
.method ul { padding-left: 4.5mm; margin: 0 0 2.5mm; }
.caveats li { margin-bottom: 1mm; }
)CSS";

struct Html {
	std::ostringstream o;
	Html& operator<<(const std::string& s) { o << s; return *this; }
	Html& operator<<(const char* s) { o << s; return *this; }
	Html& operator<<(int v) { o << v; return *this; }
};

std::string mph(int v) { return std::to_string(v) + " mph"; }
std::string f1(double v) { return fmt("%.1f", v); }
std::string thousands(long long v) {
	std::string s = std::to_string(v < 0 ? -v : v), o;
	for (size_t i = 0; i < s.size(); i++) { if (i && (s.size() - i) % 3 == 0) o += ','; o += s[i]; }
	return (v < 0 ? "-" : "") + o;
}
std::string hoursText(double h) {
	if (h < 1) return fmt("%.0f min", h * 60);
	return h < 10 ? fmt("%.1f h", h) : fmt("%.0f h", h);
}

const char* kOk = "#9aa8b8";
const char* kOver = "#e0a12a";
const char* kBad = "#c0392b";
const char* kCoverFill = "#3d6a9e";

std::string bandColor(int speed, int limit) { return speed >= limit + 10 ? kBad : speed > limit ? kOver : kOk; }

// "NW-bound (right to left)"
std::string dirName(const Study& st, bool l2r) {
	const std::string& c = l2r ? st.l2rCompass : st.r2lCompass;
	return (c.empty() ? std::string(l2r ? "Left to right" : "Right to left") : c + "-bound") + " (" + (l2r ? "left to right" : "right to left") + " on camera)";
}

// Nice upper bound for an axis.
double niceMax(double v) {
	if (v <= 0) return 1;
	double p = std::pow(10, std::floor(std::log10(v)));
	for (double m : { 1.0, 2.0, 2.5, 5.0, 10.0 }) if (m * p >= v) return m * p;
	return 10 * p;
}

// Axis for counts: a whole-number step (1, 2, 5, 10, 20, 25, 50...) and a top that's a multiple of it.
struct Axis { double top, step; };
Axis countAxis(double maxV) {
	double step = std::max(1.0, niceMax(std::max(maxV, 1.0) / 4));
	if (step != std::floor(step)) step = std::ceil(step);
	return { std::max(step, std::ceil(maxV / step - 1e-9) * step), step };
}

// Speed histogram, one bar per mph, coloured by band, with the posted limit and the 85th percentile marked.
std::string histogramSvg(const Study& st) {
	const int W = 680, H = 210, pl = 34, pr = 8, pt = 22, pb = 26;
	const auto& h = st.histogram;
	if (h.empty()) return "";
	int lo = std::min(h.front().first, st.settings.speedLimit - 5), hi = std::max(h.back().first, st.settings.speedLimit + 15);
	lo = lo / 5 * 5;
	hi = (hi + 4) / 5 * 5;
	int maxN = 0;
	for (auto& b : h) maxN = std::max(maxN, b.second);
	Axis ax = countAxis(maxN);
	int bins = hi - lo + 1;
	double bw = double(W - pl - pr) / bins;
	auto y = [&](double v) { return pt + (H - pt - pb) * (1 - v / ax.top); };
	std::ostringstream o;
	o << "<svg viewBox='0 0 " << W << " " << H << "' role='img' aria-label='Histogram of estimated speeds'>";
	for (double v = 0; v <= ax.top + 1e-9; v += ax.step) {
		o << "<line class='grid' x1='" << pl << "' x2='" << W - pr << "' y1='" << y(v) << "' y2='" << y(v) << "'/>";
		o << "<text x='" << pl - 5 << "' y='" << y(v) + 3 << "' text-anchor='end'>" << fmt("%g", v) << "</text>";
	}
	for (auto& b : h) {
		if (!b.second) continue;
		double x = pl + (b.first - lo) * bw;
		double gap = bw > 6 ? 1.0 : bw * .12;
		o << "<rect x='" << x + gap << "' y='" << y(b.second) << "' width='" << std::max(.8, bw - 2 * gap) << "' height='" << (H - pb) - y(b.second)
		  << "' fill='" << bandColor(b.first, st.settings.speedLimit) << "'><title>" << b.first << " mph: " << b.second << " vehicles</title></rect>";
	}
	o << "<line class='axis' x1='" << pl << "' x2='" << W - pr << "' y1='" << H - pb << "' y2='" << H - pb << "'/>";
	for (int m = lo; m <= hi; m += 5) {
		double x = pl + (m - lo + .5) * bw;
		o << "<line class='axis' x1='" << x << "' x2='" << x << "' y1='" << H - pb << "' y2='" << H - pb + 3 << "'/>";
		o << "<text x='" << x << "' y='" << H - pb + 13 << "' text-anchor='middle'>" << m << "</text>";
	}
	o << "<text x='" << W - pr << "' y='" << H - 2 << "' text-anchor='end'>mph</text>";
	// Limit between the limit's bar and the next: everything right of the line is over.
	double lx = pl + (st.settings.speedLimit - lo + 1) * bw;
	o << "<line class='limit' x1='" << lx << "' x2='" << lx << "' y1='" << pt - 14 << "' y2='" << H - pb << "'/>";
	// The 85th percentile: a marker over its bar.  Its label goes on the side away from the limit line's label so the
	// two never overlap (limit label to the left of its line when the 85th is to the right, and the other way round).
	double px = pl + (st.all.p85 - lo + .5) * bw;
	bool p85Right = px >= lx;
	o << "<path d='M" << px - 4 << " " << pt - 12 << "h8l-4 6z' fill='#15181d'/>";
	o << "<text class='ink' x='" << px + (p85Right ? 7 : -7) << "' y='" << pt - 6 << "' text-anchor='" << (p85Right ? "start" : "end") << "'>85th percentile " << st.all.p85 << " mph</text>";
	o << "<text class='ink' x='" << lx + (p85Right ? -5 : 5) << "' y='" << pt - 6 << "' text-anchor='" << (p85Right ? "end" : "start") << "'>Posted limit " << st.settings.speedLimit << " mph</text>";
	o << "</svg>";
	return o.str();
}

// Hour-of-day: vehicles per observed hour (bars), and below it the 85th percentile per hour on its own axis (two
// charts sharing the x axis, never one chart with two y scales).  A rate needs enough time behind it: hours observed for
// less than 15 minutes are hatched like hours never observed, so five cars in a five-minute clip don't read as 60/h.
std::string hourlySvg(const Study& st) {
	const int W = 680, pl = 34, pr = 8;
	const double kMinRateSecs = 15 * 60;
	double bw = double(W - pl - pr) / 24;
	std::ostringstream o;
	// Top: volume rate.
	const int H1 = 150, pt = 24, pb = 6;
	double rate[24];
	double maxR = 0;
	for (int h = 0; h < 24; h++) {
		rate[h] = st.hourSecs[h] >= kMinRateSecs ? st.hourN[h] / (st.hourSecs[h] / 3600.0) : -1;
		maxR = std::max(maxR, rate[h]);
	}
	Axis ax = countAxis(maxR);
	auto y = [&](double v) { return pt + (H1 - pt - pb) * (1 - v / ax.top); };
	bool anyRate = std::any_of(rate, rate + 24, [](double r) { return r >= 0; });
	if (!anyRate) {
		// e.g. one ten-minute clip: no hour has enough coverage for a rate, so a sentence instead of an empty chart.
		o << "<svg viewBox='0 0 " << W << " 30' role='img' aria-label='No hour observed long enough for a rate'><text class='ink2' x='0' y='16'>"
		  << "Vehicles per observed hour: no hour of the day was observed for 15 minutes or more, so no rate is shown.</text></svg>";
	}
	else {
		o << "<svg viewBox='0 0 " << W << " " << H1 << "' role='img' aria-label='Vehicles per observed hour by hour of day'>";
		o << "<defs><pattern id='nodata' width='5' height='5' patternUnits='userSpaceOnUse' patternTransform='rotate(45)'><line x1='0' y1='0' x2='0' y2='5' stroke='#d5d8dd' stroke-width='1.5'/></pattern></defs>";
		o << "<text class='ink' x='0' y='10' style='font-weight:600'>Vehicles per observed hour</text>";
		for (double v = 0; v <= ax.top + 1e-9; v += ax.step) {
			o << "<line class='grid' x1='" << pl << "' x2='" << W - pr << "' y1='" << y(v) << "' y2='" << y(v) << "'/>";
			o << "<text x='" << pl - 5 << "' y='" << y(v) + 3 << "' text-anchor='end'>" << fmt("%g", v) << "</text>";
		}
		for (int h = 0; h < 24; h++) {
			double x = pl + h * bw + 2;
			if (rate[h] < 0) {
				o << "<rect x='" << x << "' y='" << pt << "' width='" << bw - 4 << "' height='" << H1 - pb - pt << "' fill='url(#nodata)'><title>" << hourLabel(h) << ": "
				  << (st.hourSecs[h] > 0 ? "only " + fmt("%.0f", st.hourSecs[h] / 60) + " min observed" : std::string("not observed")) << "</title></rect>";
				continue;
			}
			o << "<rect x='" << x << "' y='" << y(rate[h]) << "' width='" << bw - 4 << "' height='" << (H1 - pb) - y(rate[h]) << "' fill='" << kCoverFill
			  << "'><title>" << hourLabel(h) << ": " << st.hourN[h] << " vehicles in " << hoursText(st.hourSecs[h] / 3600) << " observed</title></rect>";
		}
		o << "<line class='axis' x1='" << pl << "' x2='" << W - pr << "' y1='" << H1 - pb << "' y2='" << H1 - pb << "'/>";
		o << "</svg>";
	}
	// Bottom: 85th percentile per hour, hours with fewer than kMinCell vehicles left blank.
	const int H2 = 150, qt = 30, qb = 24;
	int lo = st.settings.speedLimit - 10, hi = st.settings.speedLimit + 10;
	for (int h = 0; h < 24; h++) if (st.hourP85[h] >= 0) { lo = std::min(lo, st.hourP85[h] - 3); hi = std::max(hi, st.hourP85[h] + 3); }
	lo = lo / 5 * 5;
	hi = (hi + 4) / 5 * 5;
	auto y2 = [&](double v) { return qt + (H2 - qt - qb) * (1 - (v - lo) / double(hi - lo)); };
	o << "<svg viewBox='0 0 " << W << " " << H2 << "' role='img' aria-label='85th percentile speed by hour of day'>";
	o << "<text class='ink' x='0' y='16' style='font-weight:600'>85th-percentile speed, mph</text>";
	for (int v = lo; v <= hi; v += 5) {
		o << "<line class='grid' x1='" << pl << "' x2='" << W - pr << "' y1='" << y2(v) << "' y2='" << y2(v) << "'/>";
		o << "<text x='" << pl - 5 << "' y='" << y2(v) + 3 << "' text-anchor='end'>" << v << "</text>";
	}
	double ly = y2(st.settings.speedLimit);
	o << "<line class='limit' x1='" << pl << "' x2='" << W - pr << "' y1='" << ly << "' y2='" << ly << "'/>";
	// One line through consecutive hours; an hour without a percentile breaks it rather than bridging the gap.
	std::string path;
	bool prev = false;
	for (int h = 0; h < 24; h++) {
		if (st.hourP85[h] < 0) { prev = false; continue; }
		path += (prev ? "L" : "M") + fmt("%.1f", pl + (h + .5) * bw) + " " + fmt("%.1f", y2(st.hourP85[h]));
		prev = true;
	}
	if (!path.empty()) o << "<path d='" << path << "' fill='none' stroke='#15181d' stroke-width='1.6'/>";
	for (int h = 0; h < 24; h++) {
		if (st.hourP85[h] < 0) continue;
		double x = pl + (h + .5) * bw;
		o << "<circle cx='" << x << "' cy='" << y2(st.hourP85[h]) << "' r='4.5' fill='" << bandColor(st.hourP85[h], st.settings.speedLimit)
		  << "' stroke='#fff' stroke-width='1.5'><title>" << hourLabel(h) << ": 85th percentile " << st.hourP85[h] << " mph (" << st.hourN[h] << " vehicles)</title></circle>";
	}
	o << "<line class='axis' x1='" << pl << "' x2='" << W - pr << "' y1='" << H2 - qb << "' y2='" << H2 - qb << "'/>";
	for (int h = 0; h < 24; h += 2) o << "<text x='" << pl + (h + .5) * bw << "' y='" << H2 - qb + 13 << "' text-anchor='middle'>" << hourLabel(h) << "</text>";
	o << "</svg>";
	return o.str();
}

// Weekday x hour of day, % over the limit in each cell (Monday first, as a traffic study reads).
std::string heatmapSvg(const Study& st) {
	const int W = 680, pl = 34, pr = 8, pt = 4, cell = 21;
	double cw = double(W - pl - pr) / 24;
	const int order[7] = { 1, 2, 3, 4, 5, 6, 0 };
	int H = pt + cell * 7 + 18;
	std::ostringstream o;
	o << "<svg viewBox='0 0 " << W << " " << H << "' role='img' aria-label='Share of vehicles over the limit by weekday and hour'>";
	// Sequential blue: lighter = fewer speeding.  Text flips to white on the dark half.
	static const char* ramp[] = { "#e8f0fa", "#cde2fb", "#9ec5f4", "#6da7ec", "#3987e5", "#256abf", "#184f95", "#0d366b" };
	for (int r = 0; r < 7; r++) {
		int wd = order[r];
		double y = pt + r * cell;
		o << "<text x='" << pl - 5 << "' y='" << y + cell / 2 + 3 << "' text-anchor='end'>" << std::string(kWeekdays[wd]).substr(0, 3) << "</text>";
		for (int h = 0; h < 24; h++) {
			int n = st.wdN[wd][h];
			double x = pl + h * cw;
			if (n == 0) { o << "<rect x='" << x + 1 << "' y='" << y + 1 << "' width='" << cw - 2 << "' height='" << cell - 2 << "' fill='none' stroke='#eceef1'/>"; continue; }
			if (n < kMinCell) {
				o << "<rect x='" << x + 1 << "' y='" << y + 1 << "' width='" << cw - 2 << "' height='" << cell - 2 << "' fill='#f4f5f7'><title>" << kWeekdays[wd] << " " << hourLabel(h) << ": only " << n << " vehicles</title></rect>";
				o << "<text x='" << x + cw / 2 << "' y='" << y + cell / 2 + 3 << "' text-anchor='middle' style='font-size:7px'>·</text>";
				continue;
			}
			double p = 100.0 * st.wdOver[wd][h] / n;
			int k = std::clamp(int(p / 12.5), 0, 7);
			o << "<rect x='" << x + 1 << "' y='" << y + 1 << "' width='" << cw - 2 << "' height='" << cell - 2 << "' rx='1.5' fill='" << ramp[k] << "'><title>"
			  << kWeekdays[wd] << " " << hourLabel(h) << ": " << fmt("%.0f", p) << "% of " << n << " over the limit</title></rect>";
			o << "<text x='" << x + cw / 2 << "' y='" << y + cell / 2 + 3 << "' text-anchor='middle' style='font-size:8px;fill:" << (k >= 4 ? "#fff" : "#15181d") << "'>" << fmt("%.0f", p) << "</text>";
		}
	}
	for (int h = 0; h < 24; h += 2) o << "<text x='" << pl + (h + .5) * cw << "' y='" << H - 4 << "' text-anchor='middle'>" << hourLabel(h) << "</text>";
	o << "</svg>";
	return o.str();
}

// Coverage: one row per day, 24 hour cells shaded by how much of that hour was observed.
std::string coverageSvg(const Study& st) {
	int days = int(st.toDay - st.fromDay + 1);
	if (days <= 0 || days > 120) return "";
	const int W = 680, pl = 74, pr = 50, pt = 4;
	int cell = days <= 10 ? 18 : days <= 31 ? 13 : 8;
	double cw = double(W - pl - pr) / 24;
	int H = pt + cell * days + 18;
	std::ostringstream o;
	o << "<svg viewBox='0 0 " << W << " " << H << "' role='img' aria-label='Hours observed per day'>";
	for (int i = 0; i < days; i++) {
		int64_t day = st.fromDay + i;
		double y = pt + i * cell;
		int yy, mm, dd;
		civilFromDays(day, yy, mm, dd);
		char lab[32];
		snprintf(lab, sizeof lab, "%.3s %d %s", kWeekdays[weekdayOf(day)], dd, kMonths[mm - 1]);
		if (cell >= 10 || i % 2 == 0) o << "<text x='" << pl - 6 << "' y='" << y + cell / 2.0 + 3 << "' text-anchor='end'" << (cell < 10 ? " style='font-size:7px'" : "") << ">" << lab << "</text>";
		auto it = st.dayHourSecs.find(day);
		double tot = 0;
		for (int h = 0; h < 24; h++) {
			double secs = it == st.dayHourSecs.end() ? 0 : it->second[h];
			tot += secs;
			double x = pl + h * cw;
			o << "<rect x='" << x + .5 << "' y='" << y + .5 << "' width='" << cw - 1 << "' height='" << cell - 1 << "' fill='#f1f2f4'/>";
			if (secs > 0) {
				// Width shows the fraction of the hour observed, so a 10-minute clip doesn't look like a full hour.
				double fw = (cw - 1) * std::min(1.0, secs / 3600);
				o << "<rect x='" << x + .5 << "' y='" << y + .5 << "' width='" << std::max(1.0, fw) << "' height='" << cell - 1 << "' fill='" << kCoverFill
				  << "'><title>" << lab << " " << hourLabel(h) << ": " << fmt("%.0f", secs / 60) << " min observed</title></rect>";
			}
		}
		o << "<text class='ink2' x='" << W - pr + 6 << "' y='" << y + cell / 2.0 + 3 << "'" << (cell < 10 ? " style='font-size:7px'" : "") << ">" << (tot > 0 ? hoursText(tot / 3600) : "—") << "</text>";
	}
	for (int h = 0; h <= 24; h += 3) o << "<text x='" << pl + h * cw << "' y='" << H - 4 << "' text-anchor='middle'>" << (h == 24 ? "12a" : hourLabel(h)) << "</text>";
	o << "</svg>";
	return o.str();
}

std::string wrongSvg(const Study& st) {
	const int W = 680, H = 120, pl = 34, pr = 8, pt = 8, pb = 22;
	double bw = double(W - pl - pr) / 24;
	int maxN = 1;
	for (int v : st.wrongByHour) maxN = std::max(maxN, v);
	Axis ax = countAxis(std::max(maxN, 2));
	auto y = [&](double v) { return pt + (H - pt - pb) * (1 - v / ax.top); };
	std::ostringstream o;
	o << "<svg viewBox='0 0 " << W << " " << H << "' role='img' aria-label='Wrong-way vehicles by hour of day'>";
	for (double v = 0; v <= ax.top + 1e-9; v += ax.step) {
		o << "<line class='grid' x1='" << pl << "' x2='" << W - pr << "' y1='" << y(v) << "' y2='" << y(v) << "'/>";
		o << "<text x='" << pl - 5 << "' y='" << y(v) + 3 << "' text-anchor='end'>" << fmt("%g", v) << "</text>";
	}
	for (int h = 0; h < 24; h++) {
		if (!st.wrongByHour[h]) continue;
		o << "<rect x='" << pl + h * bw + 2 << "' y='" << y(st.wrongByHour[h]) << "' width='" << bw - 4 << "' height='" << (H - pb) - y(st.wrongByHour[h]) << "' fill='" << kBad
		  << "'><title>" << hourLabel(h) << ": " << st.wrongByHour[h] << " wrong-way of " << st.hourN[h] << "</title></rect>";
	}
	o << "<line class='axis' x1='" << pl << "' x2='" << W - pr << "' y1='" << H - pb << "' y2='" << H - pb << "'/>";
	for (int h = 0; h < 24; h += 2) o << "<text x='" << pl + (h + .5) * bw << "' y='" << H - pb + 13 << "' text-anchor='middle'>" << hourLabel(h) << "</text>";
	o << "</svg>";
	return o.str();
}

void speedRow(Html& h, const std::string& name, const SpeedStats& s, bool total = false) {
	h << "<tr" << (total ? " class='total'" : "") << "><td>" << esc(name) << "</td>";
	if (!s.n) { h << "<td class='n'>0</td><td class='n muted' colspan='6'>no vehicles</td></tr>"; return; }
	h << "<td class='n'>" << thousands(s.n) << "</td><td class='n'>" << f1(s.mean) << "</td><td class='n'>" << s.median << "</td><td class='n'>" << s.p85
	  << "</td><td class='n'>" << pct(s.pctOver) << "</td><td class='n'>" << pct(s.pct10) << "</td><td class='n'>" << s.max << "</td></tr>";
}

}  // namespace

std::string renderHtml(const Study& st) {
	const Settings& s = st.settings;
	Html h;
	std::string generated = s.generated;
	if (generated.empty()) {
		std::time_t now = std::time(nullptr);
		char b[16];
		std::strftime(b, sizeof b, "%Y-%m-%d", std::localtime(&now));
		generated = b;
	}
	int64_t genDay = 0;
	std::string genText = parseDay(generated, genDay) ? niceDay(genDay) : generated;
	std::string range = st.from.empty() ? "Dates unknown" : niceRange(st.fromDay, st.toDay);
	std::string label = "Community traffic study. Estimated speeds, ±" + f1(st.band) + " mph";
	bool oneWay = st.legal != "Both";
	bool verifiedAll = st.verified[0] && st.verified[1];
	bool verifiedAny = st.verified[0] || st.verified[1];
	int sec = 0;
	auto runhead = [&](const std::string& what) {
		h << "<div class='runhead'><span>" << esc(s.siteName) << " · " << esc(range) << "</span><span>" << esc(what) << "</span></div>";
	};
	auto foot = [&]() {
		h << "<div class='foot'><span>" << esc(label) << "</span><span>VideoSpeedTracker " << esc(s.version) << " · generated " << esc(genText) << "</span></div>";
	};

	h << "<!doctype html>\n<html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
	  << "<title>" << esc(s.siteName) << " — community traffic study</title><style>" << kCss << "</style></head><body>\n";

	// ---------------------------------------------------------------- cover
	h << "<section class='sheet cover'>";
	h << "<div class='kicker'>Community traffic study</div>";
	h << "<h1>Vehicle speeds on " << esc(s.siteName) << "</h1>";
	h << "<div class='sub'>" << esc(range) << (st.hours > 0 ? " · " + hoursText(st.hours) + " observed" : "") << "</div>";
	h << "<div class='label'>" << esc(label) << "</div>";
	std::string dirs = oneWay ? std::string("One-way, ") + (st.legal == "L2R" ? st.l2rCompass : st.r2lCompass) + "-bound" : st.l2rCompass + "- and " + st.r2lCompass + "-bound";
	h << "<dl class='meta'>"
	  << "<div><dt>Posted limit</dt><dd>" << s.speedLimit << " mph</dd></div>"
	  << "<div><dt>Directions</dt><dd>" << esc(dirs) << "</dd></div>"
	  << "<div><dt>Vehicles logged</dt><dd class='num'>" << thousands(st.all.n) << "</dd></div>"
	  << "<div><dt>Generated</dt><dd>" << esc(genText) << "</dd></div></dl>";

	if (st.all.n) {
		h << "<div class='kpis'>"
		  << "<div class='kpi'><b>" << st.all.p85 << "<small>mph</small></b><span>85th-percentile speed: 85% of drivers went this fast or slower</span></div>"
		  << "<div class='kpi'><b>" << pct(st.all.pctOver) << "</b><span>over the " << s.speedLimit << " mph limit (" << thousands(st.all.over) << " vehicles)</span></div>"
		  << "<div class='kpi'><b>" << pct(st.all.pct10) << "</b><span>at " << s.speedLimit + 10 << " mph or more, 10+ over (" << thousands(st.all.over10) << ")</span></div>"
		  << "<div class='kpi'><b>" << st.all.max << "<small>mph</small></b><span>fastest vehicle logged</span></div></div>";
		h << "<h3>Findings</h3><ul class='findings'>";
		h << "<li>" << thousands(st.all.n) << " vehicles were timed over " << hoursText(st.hours) << " of observation"
		  << (!st.coverageEstimated ? "" : st.manifestRuns ? " (partly estimated, see Coverage)" : " (estimated, see Coverage)") << ". The median speed was " << st.all.median << " mph and the 85th percentile "
		  << st.all.p85 << " mph, " << (st.all.p85 > s.speedLimit ? std::to_string(st.all.p85 - s.speedLimit) + " mph over the posted limit." : "at or under the posted limit.") << "</li>";
		if (st.l2r.n && st.r2l.n)
			h << "<li>" << esc(st.l2rCompass) << "-bound: 85th percentile " << st.l2r.p85 << " mph, " << pct(st.l2r.pctOver) << " over the limit. "
			  << esc(st.r2lCompass) << "-bound: " << st.r2l.p85 << " mph, " << pct(st.r2l.pctOver) << " over.</li>";
		if (st.all.wayOver) h << "<li>" << thousands(st.all.wayOver) << " vehicles (" << pct(100.0 * st.all.wayOver / st.all.n) << ") were at " << s.wayOver << " mph or more.</li>";
		if (oneWay) h << "<li>" << thousands(st.wrongTotal) << " vehicles (" << pct(100.0 * st.wrongTotal / st.all.n) << " of traffic) went the wrong way on this one-way street.</li>";
		h << "</ul>";
	}
	else h << "<p class='lede' style='margin-top:6mm'>No vehicles were logged in this date range.</p>";

	if (verifiedAll)
		h << "<div class='status'><b>Calibration verified.</b><span>A check pass at a known speed in each direction read within 2 mph after the latest calibration (see Calibration).</span></div>";
	else if (verifiedAny)
		h << "<div class='status warn'><b>Calibration partly verified.</b><span>Only the " << esc(st.verified[0] ? st.l2rCompass : st.r2lCompass)
		  << "-bound direction has a check pass within 2 mph. Treat the other direction's speeds with more caution.</span></div>";
	else
		h << "<div class='status warn'><b>Calibration not verified.</b><span>No check pass at a known speed confirms the calibration"
		  << (st.historyFound ? "" : " (no calibration history was found)") << ". The ±" << f1(st.band) << " mph band covers frame counting only, not calibration error.</span></div>";
	h << "<p class='small'>Speeds are estimated from video by counting the frames a vehicle takes between two lines, calibrated with one pass at a known speed. "
	  << "Every figure is ±" << f1(st.band) << " mph at the limit (see Accuracy). Collected by a resident with VideoSpeedTracker; no plates or faces are recorded.</p>";
	if (!s.note.empty()) h << "<div class='note'><h3>Note from the observer</h3><p>" << esc(s.note) << "</p></div>";
	foot();
	h << "</section>\n";

	// ---------------------------------------------------------------- coverage and volumes
	h << "<section class='sheet'>";
	runhead("Coverage and volumes");
	h << "<h2><span class='num'>" << ++sec << "</span>When the camera was watching</h2>";
	h << "<p class='lede'>Counts below are for the hours actually observed. They are not scaled up to full days: a part-day of video says nothing about the hours it missed.</p>";
	if (st.coverageEstimated)
		h << "<p><b>Estimated coverage.</b> " << st.plainRuns << " of " << st.plainRuns + st.manifestRuns
		  << (st.plainRuns == 1 ? " runs has" : " runs have") << " no run manifest, so their observed time is taken as the span from the first to the last vehicle logged. Time before the first and after the last vehicle, and any outage, is not counted.</p>";
	std::string cov = coverageSvg(st);
	if (!cov.empty())
		h << "<figure>" << cov << "<figcaption><b>Hours observed per day.</b> Each cell is one hour; the filled part is the share of it the camera was recording.</figcaption>"
		  << "<div class='legend'><span><i style='background:" << kCoverFill << "'></i>Observed</span><span><i style='background:#f1f2f4'></i>Not observed</span></div></figure>";
	if (!st.gaps.empty()) {
		h << "<h3>Gaps of an hour or more</h3><table><tr><th>From</th><th>To</th><th class='n'>Length</th></tr>";
		size_t shown = 0;
		for (const Interval& g : st.gaps) {
			if (++shown > 12) break;
			h << "<tr><td class='tab'>" << niceTime(g.from) << "</td><td class='tab'>" << niceTime(g.to) << "</td><td class='n'>" << hoursText((g.to - g.from) / 3600.0) << "</td></tr>";
		}
		h << "</table>";
		if (st.gaps.size() > 12) h << "<p class='small'>and " << st.gaps.size() - 12 << " more.</p>";
	}
	h << "<h3>Volumes</h3><table><tr><th style='width:36%'>What</th><th class='n'>Vehicles</th><th>Notes</th></tr>";
	h << "<tr><td>Timed and logged in this range</td><td class='n'>" << thousands(st.all.n + st.flaggedExcluded) << "</td><td>"
	  << (st.flaggedExcluded ? thousands(st.flaggedExcluded) + " flagged as implausible and left out of the statistics" : std::string("every logged vehicle is in the statistics")) << "</td></tr>";
	if (st.manifestRuns) {
		h << "<tr><td>Below the logging minimum</td><td class='n'>" << thousands(st.mBelow) << "</td><td>timed under " << st.minLoggedSpeed << " mph (turning, parking, stopping) and not logged</td></tr>";
		h << "<tr><td>Seen but not timed</td><td class='n'>" << thousands(st.mUntimed) << "</td><td>entered the zone but couldn't be timed: overlapping vehicles, occlusion</td></tr>";
	}
	h << "<tr><td>Vehicles per observed hour</td><td class='n'>" << (st.hours > 0 ? f1((st.all.n + st.flaggedExcluded) / st.hours) : std::string("—")) << "</td><td>logged vehicles ÷ hours observed</td></tr>";
	h << "</table>";
	if (st.manifestRuns) {
		int seen = st.mLogged + st.mBelow + st.mUntimed;
		h << "<p><b>Capture rate.</b> In the " << st.manifestRuns << " run" << (st.manifestRuns > 1 ? "s" : "") << " with a manifest, VST saw "
		  << thousands(seen) << " vehicles enter the timing zone and logged " << thousands(st.mLogged) << " (" << pct(seen ? 100.0 * st.mLogged / seen : 0) << "). "
		  << "Of the rest, " << thousands(st.mBelow) << " were slower than " << st.minLoggedSpeed << " mph and " << thousands(st.mUntimed) << " couldn't be timed. "
		  << "Counts are for whole runs.</p>";
	}
	else h << "<p class='small'>Vehicles below the logging minimum and vehicles that couldn't be timed are not known for these runs (no run manifest), so no capture rate can be given.</p>";
	foot();
	h << "</section>\n";

	// ---------------------------------------------------------------- speeds
	h << "<section class='sheet'>";
	runhead("Speeds");
	h << "<h2><span class='num'>" << ++sec << "</span>Speeds</h2>";
	h << "<table><tr><th>Direction</th><th class='n'>Vehicles</th><th class='n'>Mean</th><th class='n'>Median</th><th class='n'>85th pct.</th><th class='n'>Over " << s.speedLimit
	  << "</th><th class='n'>" << s.speedLimit + 10 << "+</th><th class='n'>Max</th></tr>";
	speedRow(h, dirName(st, true), st.l2r);
	speedRow(h, dirName(st, false), st.r2l);
	speedRow(h, "Both directions", st.all, true);
	h << "</table><p class='small'>Speeds in mph. 85th percentile by nearest rank. “Over” means above the posted limit; “" << s.speedLimit + 10 << "+” means 10 mph or more over it.</p>";
	if (st.all.n) {
		h << "<figure class='keep'>" << histogramSvg(st) << "<figcaption><b>How fast vehicles went.</b> Vehicles at each speed, both directions; the dashed line is the posted limit and the marker the 85th percentile ("
		  << st.all.p85 << " mph).</figcaption><div class='legend'><span><i style='background:" << kOk << "'></i>At or under " << s.speedLimit << "</span><span><i style='background:" << kOver
		  << "'></i>Over the limit</span><span><i style='background:" << kBad << "'></i>" << s.speedLimit + 10 << " mph or more</span><span><i class='line'></i>Posted limit</span></div></figure>";
		h << "<figure class='keep'>" << hourlySvg(st) << "<figcaption><b>By hour of day.</b> Top: vehicles per observed hour (hatched: not observed for at least 15 minutes). Bottom: 85th-percentile speed, for hours with at least "
		  << kMinCell << " vehicles; the dashed line is the limit.</figcaption></figure>";
	}
	foot();
	h << "</section>\n";

	// The weekday heatmap (only with more than one weekday) and the wrong-way count (one-way streets only) are short,
	// so they share a page when both are present.
	bool heat = st.all.n && st.weekdays > 1;
	if (heat || oneWay) {
		h << "<section class='sheet'>";
		runhead(heat && oneWay ? "Patterns and wrong-way drivers" : heat ? "Speeding by day and hour" : "Wrong-way drivers");
	}
	if (heat) {
		h << "<h2><span class='num'>" << ++sec << "</span>When speeding happens</h2>";
		h << "<p class='lede'>Share of vehicles over the " << s.speedLimit << " mph limit for each weekday and hour. Hours with fewer than " << kMinCell
		  << " vehicles show a dot; empty cells were not observed or had no traffic.</p>";
		h << "<figure>" << heatmapSvg(st) << "<figcaption><b>% over the limit, by weekday and hour of day.</b></figcaption>"
		  << "<div class='legend'><span><i style='background:#e8f0fa;outline:.5pt solid #d5d8dd'></i>0–12%</span><span><i style='background:#9ec5f4'></i>25–37%</span>"
		  << "<span><i style='background:#3987e5'></i>50–62%</span><span><i style='background:#184f95'></i>75–87%</span><span><i style='background:#0d366b'></i>88–100%</span></div></figure>";
	}
	if (oneWay) {
		std::string legalC = st.legal == "L2R" ? st.l2rCompass : st.r2lCompass;
		h << "<h2" << (heat ? " style='margin-top:9mm'" : "") << "><span class='num'>" << ++sec << "</span>Wrong-way drivers</h2>";
		h << "<p class='lede'>The street is one-way " << esc(legalC) << "-bound (LegalDirection " << esc(st.legal) << "). Every vehicle timed going the other way is counted here. "
		  << thousands(st.wrongTotal) << " of " << thousands(st.all.n) << " vehicles (" << pct(st.all.n ? 100.0 * st.wrongTotal / st.all.n : 0) << ") went the wrong way.</p>";
		h << "<figure>" << wrongSvg(st) << "<figcaption><b>Wrong-way vehicles by hour of day.</b></figcaption></figure>";
		h << "<table><tr><th style='width:40%'>Hour</th><th class='n'>Vehicles</th><th class='n'>Wrong way</th><th class='n'>Share of traffic</th></tr>";
		for (int hr = 0; hr < 24; hr++) {
			if (!st.hourN[hr]) continue;
			h << "<tr><td>" << hourLabel(hr) << "–" << hourLabel((hr + 1) % 24) << "</td><td class='n'>" << st.hourN[hr] << "</td><td class='n'>" << st.wrongByHour[hr]
			  << "</td><td class='n'>" << pct(100.0 * st.wrongByHour[hr] / st.hourN[hr]) << "</td></tr>";
		}
		h << "<tr class='total'><td>All hours</td><td class='n'>" << thousands(st.all.n) << "</td><td class='n'>" << thousands(st.wrongTotal) << "</td><td class='n'>"
		  << pct(st.all.n ? 100.0 * st.wrongTotal / st.all.n : 0) << "</td></tr></table>";
	}
	if (heat || oneWay) {
		foot();
		h << "</section>\n";
	}

	// ---------------------------------------------------------------- before and after (optional)
	if (st.hasCompare) {
		h << "<section class='sheet'>";
		runhead("Before and after");
		h << "<h2><span class='num'>" << ++sec << "</span>Before and after</h2>";
		auto periodName = [](const PeriodSummary& p) {
			int64_t a, b;
			return parseDay(p.from, a) && parseDay(p.to, b) ? niceRange(a, b) : std::string("no data");
		};
		h << "<p class='lede'>The key figures for two periods side by side: the comparison period first, then this report's. Each is worked out on its own, "
		  << "from the vehicles and the hours observed in that period.</p><table><tr><th style='width:40%'></th><th class='n'>"
		  << esc(periodName(st.compare)) << "</th><th class='n'>" << esc(periodName(st.main)) << "</th></tr>";
		auto row = [&](const std::string& name, const std::string& a, const std::string& b) { h << "<tr><td>" << esc(name) << "</td><td class='n'>" << a << "</td><td class='n'>" << b << "</td></tr>"; };
		const PeriodSummary &a = st.compare, &b = st.main;
		row("Hours observed", hoursText(a.hours), hoursText(b.hours));
		row("Vehicles logged", thousands(a.all.n), thousands(b.all.n));
		row("Vehicles per observed hour", f1(a.perHour), f1(b.perHour));
		row("Median speed", mph(a.all.median), mph(b.all.median));
		row("85th-percentile speed", mph(a.all.p85), mph(b.all.p85));
		row("Over " + std::to_string(s.speedLimit) + " mph", pct(a.all.pctOver), pct(b.all.pctOver));
		row(std::to_string(s.speedLimit + 10) + " mph or more", pct(a.all.pct10), pct(b.all.pct10));
		row("Fastest", mph(a.all.max), mph(b.all.max));
		if (oneWay) row("Wrong way", thousands(a.wrongWay), thousands(b.wrongWay));
		h << "</table><p class='small'>Both periods use the same method, limit and calibration record. A change in speed smaller than the ±" << f1(st.band)
		  << " mph band is within the measurement error.</p>";
		foot();
		h << "</section>\n";
	}

	// ---------------------------------------------------------------- calibration and accuracy
	h << "<section class='sheet'>";
	runhead("Calibration and accuracy");
	h << "<h2><span class='num'>" << ++sec << "</span>Calibration and accuracy</h2>";
	h << "<h3>Calibration record</h3>";
	if (!st.historyFound)
		h << "<p><b>No calibration history.</b> This setup has no calibration-history.csv (it is kept from VST 1.2 on), so there is no dated record of how the calibration was set or checked. "
		  << "The calibration is <b>unverified</b>.</p>";
	else if (st.calEvents.empty())
		h << "<p><b>No calibration events for this configuration</b>" << (st.historyOtherConfigs ? " (the history has " + std::to_string(st.historyOtherConfigs) + " for other configurations)" : std::string())
		  << ". The calibration is <b>unverified</b>.</p>";
	else {
		h << "<table><tr><th>Date</th><th>Event</th><th>Direction</th><th class='n'>Driven</th><th class='n'>Frames</th><th class='n'>VST read</th><th class='n'>Error</th></tr>";
		for (const CalEvent& e : st.calEvents) {
			int64_t t;
			int off;
			bool has;
			std::string when = e.when;
			if (parseIso(e.when, t, off, has)) {
				char hm[8];
				snprintf(hm, sizeof hm, " %02d:%02d", hourOf(t), int((t - dayOf(t) * 86400) / 60 % 60));
				when = niceDay(dayOf(t)) + hm;
			}
			std::string dname = e.dir == "L2R" ? st.l2rCompass + "-bound" : st.r2lCompass + "-bound";
			h << "<tr><td class='tab'>" << esc(when) << "</td><td>" << (e.kind == "apply" ? "Calibrated" : "Check pass") << "</td><td>" << esc(dname) << "</td><td class='n'>" << fmt("%g", e.driven)
			  << " mph</td><td class='n'>" << e.frames << " @ " << fmt("%g", e.fps) << " fps</td><td class='n'>" << (e.hasMeasured ? f1(e.measured) + " mph" : "—") << "</td><td class='n'>"
			  << (e.kind == "check" ? fmt("%+.1f", e.error) : std::string("—")) << "</td></tr>";
		}
		h << "</table>";
	}
	h << "<table><tr><th style='width:22%'>Direction</th><th>State</th><th class='n'>Latest check error</th></tr>";
	for (int k = 0; k < 2; k++) {
		std::string dname = (k == 0 ? st.l2rCompass : st.r2lCompass) + "-bound";
		std::string state = st.verified[k] ? "Verified (check within 2 mph)"
			: st.checkMismatch[k] ? "Not checked: the runs used CalibrationFrames " + std::to_string(st.usedCalFrames[k]) + ", the check tested "
				+ std::to_string(st.checkCalFrames[k]) + " (VST.cfg changed without a calibration in VST)"
			: st.hasCheck[k] ? "Not verified: latest check off by more than 2 mph" : "Not verified: no check pass since the last calibration";
		h << "<tr><td>" << esc(dname) << "</td><td>" << (st.verified[k] ? "" : "<b>") << state << (st.verified[k] ? "" : "</b>") << "</td><td class='n'>"
		  << (st.hasCheck[k] ? fmt("%+.1f mph", st.checkError[k]) : std::string("—")) << "</td></tr>";
	}
	h << "</table>";
	h << "<h3>Error band: ±" << f1(st.band) << " mph at the limit</h3>";
	h << "<table><tr><th style='width:22%'>Source of error</th><th class='n' style='width:10%'>± mph</th><th>How it is worked out</th></tr>";
	h << "<tr><td>Frame counting</td><td class='n'>" << f1(st.quantMph) << "</td><td>A car at " << s.speedLimit << " mph takes about " << fmt("%.0f", std::min(st.framesAtLimit[0], st.framesAtLimit[1]))
	  << " frames between the lines (the fewer of the two directions). Missing the crossing by one frame changes the estimate by " << s.speedLimit << " ÷ (frames − 1).</td></tr>";
	h << "<tr><td>Calibration check</td><td class='n'>" << (st.calMph > 0 ? f1(st.calMph) : std::string("—")) << "</td><td>"
	  << (st.calMph > 0 || (st.hasCheck[0] && !st.checkMismatch[0]) || (st.hasCheck[1] && !st.checkMismatch[1])
			? "The larger error of the latest check pass in each direction that tested the calibration in use."
			: "No check pass tested the calibration in use, so its own error is unknown and not included.") << "</td></tr>";
	h << "<tr><td>Rounding</td><td class='n'>0.5</td><td>Speeds are logged as whole mph.</td></tr>";
	h << "<tr class='total'><td>Stated band</td><td class='n'>" << f1(st.band) << "</td><td>The three added together, worst direction, rounded up.</td></tr></table>";
	// At a higher speed v the car crosses in N * limit / v frames, so one frame is worth v / (frames - 1).
	double nAt10 = std::min(st.framesAtLimit[0], st.framesAtLimit[1]) * s.speedLimit / (s.speedLimit + 10.0);
	h << "<p class='small'>Faster vehicles cross in fewer frames, so the band grows with speed: at " << s.speedLimit + 10 << " mph it is about ±"
	  << f1(std::ceil(((s.speedLimit + 10.0) / std::max(1.5, nAt10 - 1) + st.calMph + .5) * 10 - 1e-9) / 10) << " mph.</p>";
	foot();
	h << "</section>\n";

	// ---------------------------------------------------------------- example frames
	if (!st.snapshots.empty()) {
		h << "<section class='sheet'>";
		runhead("Example frames");
		h << "<h2><span class='num'>" << ++sec << "</span>Example frames</h2>";
		h << "<p class='lede'>A few of the fastest vehicles, as the camera saw them at the second timing line. Frames are reduced in resolution and the lower part of each vehicle (plate height) is blurred.</p><div class='snaps'>";
		for (const Snapshot& sn : st.snapshots) {
			int64_t t;
			int off;
			bool has;
			std::string when = parseIso(sn.when, t, off, has) ? niceTime(t) : "";
			h << "<figure><img alt='Vehicle at an estimated " << sn.speed << " mph' src='data:image/jpeg;base64," << base64(sn.jpeg) << "'><figcaption><b>" << sn.speed << " mph</b> "
			  << esc((sn.l2r ? st.l2rCompass : st.r2lCompass) + "-bound") << (when.empty() ? "" : " · " + when) << "</figcaption></figure>";
		}
		h << "</div>";
		foot();
		h << "</section>\n";
	}

	// ---------------------------------------------------------------- method appendix
	h << "<section class='sheet method'>";
	runhead("Method");
	h << "<h2><span class='num'>" << ++sec << "</span>Method</h2>";
	h << "<h3>How VST measures speed</h3><p>A fixed camera looks across the street. VideoSpeedTracker (VST) finds moving vehicles by comparing consecutive frames, follows each one's front bumper, "
	  << "and counts the frames it takes to travel between two vertical lines drawn on the picture. Speed is the calibration speed scaled by the ratio of frame counts: "
	  << "<i>speed = calibration speed × calibration frames ÷ frames counted</i>, corrected for the video's frame rate. It measures time over a fixed distance; it does not use radio or laser.</p>";
	h << "<h3>Calibration</h3><p>The distance between the lines is never measured directly. Instead a car drives through at a known speed (on cruise control) and VST records how many frames it took, "
	  << "separately for each direction because the near and far lanes are at different distances from the camera. A later check pass at a known speed shows how far off the calibration reads. "
	  << "A direction counts as verified when its latest check after its latest calibration is within 2 mph.</p>";
	h << "<h3>What is excluded and why</h3><ul>";
	h << "<li>Vehicles timed under " << st.minLoggedSpeed << " mph (the logging minimum): mostly turning, parking or stopping, not free-flowing traffic.</li>";
	h << "<li>Vehicles that couldn't be timed cleanly (two vehicles overlapping, a vehicle hidden behind another): their crossing can't be measured.</li>";
	h << "<li>Speeds flagged as implausible (more than 20 mph over the “way over” setting): usually two vehicles merged into one; they're left out of every statistic.</li>";
	h << "<li>Vehicles tracked across a camera dropout: dropped, so no speed is ever measured across a gap.</li></ul>";
	h << "<h3>Statistics</h3><p><b>Percentiles use the nearest-rank method:</b> sort the n speeds; the p-th percentile is the value at position ⌈p/100 × n⌉. "
	  << "The 85th percentile is the speed at or below which 85% of vehicles travel, the figure traffic engineers use to review a speed limit. "
	  << "“Over the limit” means strictly above " << s.speedLimit << " mph. Hour-of-day figures use the local clock time of each vehicle, and hourly percentiles are shown only for hours with at least " << kMinCell << " vehicles. "
	  << "Coverage is the time between each run's start and end minus outages (from the run manifest); without a manifest it is the time from the first to the last vehicle.</p>";
	h << "<h3>Privacy</h3><p>VST stores a speed, a direction and a time per vehicle. It does not read plates or recognise faces; the location is given only as a street and block.</p>";
	if (!st.caveats.empty()) {
		h << "<h3>Limitations of this data</h3><ul class='caveats'>";
		for (const std::string& c : st.caveats) h << "<li>" << esc(c) << "</li>";
		h << "</ul>";
	}
	h << "<h3>Data sources</h3><table><tr><th>Stats file</th><th>Manifest</th><th class='n'>Rows</th><th>Notes</th></tr>";
	{
		std::vector<int> rowsPer(st.runs.size(), 0);
		for (const Obs& o : st.obs) rowsPer[o.run]++;
		int listed = 0;
		for (size_t i = 0; i < st.runs.size(); i++) {
			const Run& r = st.runs[i];
			if (!rowsPer[i] && !(r.timed && r.span.to >= st.fromDay * 86400 && r.span.from < (st.toDay + 1) * 86400)) continue;
			if (++listed > 25) continue;
			std::string notes = r.columns == 13 ? "13-column csv" : "";
			if (r.timesDerived) notes += std::string(notes.empty() ? "" : "; ") + "times from Date/Time";
			if (!r.vst.empty()) notes += std::string(notes.empty() ? "" : "; ") + "VST " + r.vst;
			if (r.manifest && !r.complete) notes += std::string(notes.empty() ? "" : "; ") + "in progress";
			h << "<tr><td class='tab'>" << esc(r.name) << "</td><td>" << (r.manifest ? "yes" : "no") << "</td><td class='n'>" << rowsPer[i] << "</td><td>" << esc(notes) << "</td></tr>";
		}
		if (listed > 25) h << "<tr><td colspan='4' class='muted'>and " << listed - 25 << " more files</td></tr>";
	}
	h << "</table>";
	h << "<p class='small'>Made with VideoSpeedTracker " << esc(s.version) << " (BSD 3-Clause, github.com/joekiller/VideoSpeedTracker) on " << esc(genText) << ".</p>";
	foot();
	h << "</section>\n</body></html>\n";
	return h.o.str();
}

bool writeReport(const Input& in, const Settings& s, std::string& path, std::string& html, std::string& error) {
	Study st = analyze(in, s);
	html = renderHtml(st);
	fs::path dir = fs::path(in.dataDir) / "Reports";
	std::error_code ec;
	fs::create_directories(dir, ec);
	std::string from = st.from.empty() ? "undated" : st.from, to = st.to.empty() ? "undated" : st.to;
	fs::path out = dir / ("report_" + slug(s.siteName) + "_" + from + "_" + to + ".html");
	// Temp file then rename, so a half-written report never replaces a good one.
	fs::path tmp = out;
	tmp += ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary);
		f << html;
		if (!f) { error = "Can't write " + tmp.string(); return false; }
	}
	fs::rename(tmp, out, ec);
	if (ec) { error = "Can't write " + out.string() + ": " + ec.message(); return false; }
	path = out.string();
	return true;
}

std::string blurForReport(const cv::Mat& bgr) {
	if (bgr.empty()) return "";
	cv::Mat img;
	// Shrink to at most 480 px wide: enough to show the vehicle and the street, too little for a face.
	double k = std::min(1.0, 480.0 / bgr.cols);
	cv::resize(bgr, img, cv::Size(), k, k, cv::INTER_AREA);
	// The snapshot is a crop at the analysis box's height around the vehicle, so the vehicle box spans it top to
	// bottom.  Plates sit in the lower part of the box: blur the bottom 45% hard, then pixelate it.
	int y0 = int(img.rows * .55);
	cv::Mat band = img.rowRange(y0, img.rows);
	int kernel = std::max(15, (img.cols / 12) | 1);
	cv::GaussianBlur(band, band, cv::Size(kernel, kernel), 0);
	cv::Mat small;
	cv::resize(band, small, cv::Size(std::max(1, band.cols / 16), std::max(1, band.rows / 8)), 0, 0, cv::INTER_AREA);
	cv::resize(small, band, band.size(), 0, 0, cv::INTER_NEAREST);
	std::vector<uchar> buf;
	if (!cv::imencode(".jpg", img, buf, { cv::IMWRITE_JPEG_QUALITY, 82 })) return "";
	return std::string(buf.begin(), buf.end());
}

}  // namespace report
