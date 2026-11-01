#include "Dataset.h"
#include "Archive.h"
#include "Sha256.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace dataset {

namespace {
constexpr double kFtPerSecPerMph = 5280.0 / 3600.0;
constexpr double kZ95Two = 1.96;      // two-sided 95%: the band a dataset promises
constexpr double kZ95One = 1.645;     // one-sided 95%: each of the two tests in TOST
constexpr double kSigmaUnknown = 0.06;   // CalEvidence kSigmaEarlier: a calibration whose method isn't on record

bool writeText(const fs::path& p, const std::string& text) {
	std::ofstream out(p, std::ios::binary | std::ios::trunc);
	out << text;
	return bool(out);
}
std::string readText(const fs::path& p) {
	std::ifstream in(p, std::ios::binary);
	std::ostringstream s;
	s << in.rdbuf();
	return s.str();
}
std::string fixed(double v, int places) {
	char b[64];
	snprintf(b, sizeof b, "%.*f", places, v);
	return b;
}
// A csv cell: quoted when it holds a comma, quote or newline.
std::string cell(const std::string& s) {
	if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
	std::string q = "\"";
	for (char c : s) q += c == '"' ? std::string("\"\"") : std::string(1, c);
	return q + "\"";
}
std::vector<std::string> splitCsv(const std::string& line) {
	std::vector<std::string> out;
	std::string cur;
	bool quoted = false;
	for (size_t i = 0; i < line.size(); i++) {
		char c = line[i];
		if (quoted) {
			if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; i++; }
			else if (c == '"') quoted = false;
			else cur += c;
		}
		else if (c == '"') quoted = true;
		else if (c == ',') { out.push_back(cur); cur.clear(); }
		else if (c != '\r') cur += c;
	}
	out.push_back(cur);
	return out;
}
std::string isoStamp(const std::string& iso) {   // "2026-11-12T09:30:00" -> "2026-11-12_0930" for a file name
	std::string d = iso.substr(0, std::min<size_t>(10, iso.size()));
	std::string t = iso.size() >= 16 ? iso.substr(11, 2) + iso.substr(14, 2) : "";
	return t.empty() ? d : d + "_" + t;
}
std::string shortDay(const std::string& iso) {
	static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	if (iso.size() < 10) return iso;
	int m = std::atoi(iso.substr(5, 2).c_str()), d = std::atoi(iso.substr(8, 2).c_str());
	if (m < 1 || m > 12) return iso;
	return std::string(months[m - 1]) + " " + std::to_string(d) + ", " + iso.substr(0, 4);
}
bool traffic(const Point& p) { return !p.ownDrive && !p.flagged; }

json calJson(const Calibration& c) {
	return { { "zoneFt", { { "L2R", std::round(c.zoneFt[0] * 1000) / 1000 }, { "R2L", std::round(c.zoneFt[1] * 1000) / 1000 } } },
		{ "sigma", { { "L2R", c.sigma[0] }, { "R2L", c.sigma[1] } } },
		{ "band95", { { "L2R", c.band(0) }, { "R2L", c.band(1) } } }, { "how", c.how } };
}
Calibration calFrom(const json& j) {
	Calibration c;
	if (!j.is_object()) return c;
	const json& z = j.value("zoneFt", json::object());
	const json& s = j.value("sigma", json::object());
	c.zoneFt[0] = z.value("L2R", 0.0); c.zoneFt[1] = z.value("R2L", 0.0);
	c.sigma[0] = s.value("L2R", 0.0); c.sigma[1] = s.value("R2L", 0.0);
	c.how = j.value("how", std::string());
	return c;
}
json summaryJson(const Summary& s) {
	return { { "cars", s.cars }, { "over", s.over }, { "wayOver", s.wayOver }, { "wrongWay", s.wrongWay }, { "p85", s.p85 } };
}
Summary summaryFrom(const json& j) {
	Summary s;
	if (!j.is_object()) return s;
	s.cars = j.value("cars", 0); s.over = j.value("over", 0); s.wayOver = j.value("wayOver", 0);
	s.wrongWay = j.value("wrongWay", 0); s.p85 = j.value("p85", 0);
	return s;
}
json testJson(const Test& t) {
	return { { "delta", t.delta }, { "sigmaNew", t.sigmaNew }, { "tolerance", t.tolerance }, { "lo", t.lo }, { "hi", t.hi },
		{ "verdict", verdictName(t.verdict) } };
}
Test testFrom(const json& j) {
	Test t;
	if (!j.is_object()) return t;
	t.delta = j.value("delta", 0.0); t.sigmaNew = j.value("sigmaNew", 0.0); t.tolerance = j.value("tolerance", 0.0);
	t.lo = j.value("lo", 0.0); t.hi = j.value("hi", 0.0); t.verdict = verdictFrom(j.value("verdict", std::string()));
	return t;
}
json checkJson(const Check& c) {
	return { { "when", c.when }, { "verdict", verdictName(c.verdict) }, { "calibration", calJson(c.cal) },
		{ "test", { { "L2R", testJson(c.test[0]) }, { "R2L", testJson(c.test[1]) } } },
		{ "before", summaryJson(c.before) }, { "after", summaryJson(c.after) },
		{ "meanDiff", std::round(c.meanDiff * 100) / 100 }, { "maxDiff", std::round(c.maxDiff * 100) / 100 }, { "file", c.file } };
}
Check checkFrom(const json& j) {
	Check c;
	c.when = j.value("when", std::string());
	c.verdict = verdictFrom(j.value("verdict", std::string()));
	c.cal = calFrom(j.value("calibration", json::object()));
	const json& t = j.value("test", json::object());
	if (t.is_object()) { c.test[0] = testFrom(t.value("L2R", json::object())); c.test[1] = testFrom(t.value("R2L", json::object())); }
	c.before = summaryFrom(j.value("before", json::object()));
	c.after = summaryFrom(j.value("after", json::object()));
	c.meanDiff = j.value("meanDiff", 0.0);
	c.maxDiff = j.value("maxDiff", 0.0);
	c.file = j.value("file", std::string());
	return c;
}
json infoJson(const Info& in) {
	return { { "schema", in.schema }, { "id", in.id }, { "name", in.name }, { "version", in.version }, { "correctionOf", in.correctionOf },
		{ "site", in.site }, { "published", in.published }, { "vst", in.vst }, { "speedLimit", in.speedLimit }, { "wayOver", in.wayOver },
		{ "directions", { { "L2R", in.l2rCompass }, { "R2L", in.r2lCompass }, { "legal", in.legal } } },
		{ "first", in.first }, { "last", in.last }, { "calibrationSet", in.calibrationSet },
		{ "calibration", calJson(in.cal) }, { "summary", summaryJson(in.summary) }, { "points", "points.csv" },
		{ "pointCount", in.points.size() } };
}

const char* kPointsHeader = "n,passed,direction,compass,wrongWay,zoneSeconds,speedMph,trend,entryMph,exitMph,accelMphPerSec,mphIn20ft,lengthFt,flagged,ownDrive,run,frame";
}  // namespace

const char* verdictName(Verdict v) {
	switch (v) {
	case Verdict::Accurate: return "accurate";
	case Verdict::Review: return "review";
	case Verdict::Tainted: return "tainted";
	default: return "";
	}
}
const char* verdictLabel(Verdict v) {
	switch (v) {
	case Verdict::Accurate: return "Still accurate";
	case Verdict::Review: return "Under review";
	case Verdict::Tainted: return "Tainted: needs correction";
	default: return "Not re-checked";
	}
}
Verdict verdictFrom(const std::string& n) {
	return n == "accurate" ? Verdict::Accurate : n == "review" ? Verdict::Review : n == "tainted" ? Verdict::Tainted : Verdict::None;
}

double Calibration::band(int dir) const {
	double s = sigma[dir == 0 ? 0 : 1];
	return kZ95Two * (s > 0 ? s : kSigmaUnknown);
}

Summary summarize(const std::vector<Point>& pts, int limit, int wayOver) {
	Summary s;
	std::vector<int> sp;
	for (const Point& p : pts) {
		if (!traffic(p)) continue;
		s.cars++;
		s.over += p.speed > limit;
		s.wayOver += p.speed >= wayOver;
		s.wrongWay += p.wrongWay;
		sp.push_back(p.speed);
	}
	std::sort(sp.begin(), sp.end());
	// The nearest-rank 85th percentile, as review.html and the street report compute it.
	if (!sp.empty()) s.p85 = sp[size_t(std::max(0, int(std::ceil(0.85 * sp.size())) - 1))];
	return s;
}

Test equivalence(double publishedFt, double publishedSigma, double newFt, double newSigma) {
	Test t;
	if (!(publishedFt > 0) || !(newFt > 0)) return t;
	t.delta = newFt / publishedFt - 1;
	t.sigmaNew = newSigma > 0 ? newSigma : kSigmaUnknown;
	t.tolerance = kZ95Two * (publishedSigma > 0 ? publishedSigma : kSigmaUnknown);
	t.lo = t.delta - kZ95One * t.sigmaNew;
	t.hi = t.delta + kZ95One * t.sigmaNew;
	double size = std::fabs(t.delta);
	if (size + kZ95One * t.sigmaNew <= t.tolerance) t.verdict = Verdict::Accurate;
	else if (size - kZ95One * t.sigmaNew > t.tolerance) t.verdict = Verdict::Tainted;
	else t.verdict = Verdict::Review;
	return t;
}

std::string datasetJson(const Info& in) { return infoJson(in).dump(1, '\t') + "\n"; }

std::string pointsCsv(const Info& in) {
	std::string out = std::string(kPointsHeader) + "\n";
	for (const Point& p : in.points) {
		out += std::to_string(p.n) + "," + cell(p.passed) + "," + (p.l2r ? "L2R" : "R2L") + "," + cell(p.compass) + "," + (p.wrongWay ? "1" : "0") + ","
			+ (p.zoneSeconds > 0 ? fixed(p.zoneSeconds, 4) : "") + "," + std::to_string(p.speed) + "," + cell(p.trend) + ","
			+ (p.trend.empty() ? ",,," : std::to_string(p.entryMph) + "," + std::to_string(p.exitMph) + "," + fixed(p.accel, 1) + "," + std::to_string(p.mphIn20ft)) + ","
			+ (p.lengthFt > 0 ? fixed(p.lengthFt, 1) : "") + "," + (p.flagged ? "1" : "0") + "," + std::to_string(p.ownDrive) + "," + cell(p.run) + ","
			+ std::to_string(p.frame) + "\n";
	}
	return out;
}

std::string statusJson(const Info& in) {
	json checks = json::array();
	for (const Check& c : in.checks) checks.push_back(checkJson(c));
	return json({ { "schema", in.schema }, { "id", in.id }, { "status", verdictName(in.status()) }, { "checks", checks } }).dump(1, '\t') + "\n";
}

std::string coverJson(const Info& in) {
	json j = infoJson(in);
	json checks = json::array();
	for (const Check& c : in.checks) checks.push_back(checkJson(c));
	j["checks"] = checks;
	j["status"] = verdictName(in.status());
	j["statusLabel"] = verdictLabel(in.status());
	j["sentence"] = sentence(in);
	j["accuracy"] = accuracyWords(in.cal, in.speedLimit);
	return j.dump();
}

bool isDataset(const std::string& dir) { std::error_code ec; return fs::is_regular_file(fs::path(dir) / "dataset.json", ec); }

bool read(const std::string& dirIn, Info& out, std::string& error) {
	fs::path dir(dirIn);
	if (dir.filename() == "dataset.json") dir = dir.parent_path();
	std::ifstream in(dir / "dataset.json", std::ios::binary);
	if (!in) { error = "No dataset.json in " + dir.string() + ". Is this a published dataset?"; return false; }
	json j = json::parse(in, nullptr, false);
	if (j.is_discarded() || !j.is_object()) { error = (dir / "dataset.json").string() + " isn't readable JSON."; return false; }
	std::string schema = j.value("schema", std::string());
	if (schema.rfind("vst-dataset/", 0) != 0) { error = (dir / "dataset.json").string() + " isn't a VST dataset."; return false; }
	if (std::atoi(schema.c_str() + 12) > 1) {
		error = "This dataset uses a newer format (" + schema + ") than this VST reads. Update VST to open it.";
		return false;
	}
	out = Info();
	try {
		out.dir = dir.string();
		out.schema = schema;
		out.id = j.value("id", dir.filename().string());
		out.name = j.value("name", std::string());
		out.version = j.value("version", 1);
		out.correctionOf = j.value("correctionOf", std::string());
		out.site = j.value("site", std::string());
		out.published = j.value("published", std::string());
		out.vst = j.value("vst", std::string());
		out.speedLimit = j.value("speedLimit", 25);
		out.wayOver = j.value("wayOver", 35);
		const json& d = j.value("directions", json::object());
		if (d.is_object()) {
			out.l2rCompass = d.value("L2R", out.l2rCompass);
			out.r2lCompass = d.value("R2L", out.r2lCompass);
			out.legal = d.value("legal", out.legal);
		}
		out.first = j.value("first", std::string());
		out.last = j.value("last", std::string());
		out.calibrationSet = j.value("calibrationSet", 0);
		out.cal = calFrom(j.value("calibration", json::object()));
		out.summary = summaryFrom(j.value("summary", json::object()));
	}
	catch (const json::exception&) { error = (dir / "dataset.json").string() + " has a field of the wrong type."; return false; }

	// points.csv: by header name, so a reader of vst-dataset/1 copes with columns added later in /1.
	std::ifstream pc(dir / j.value("points", std::string("points.csv")), std::ios::binary);
	if (!pc) { error = "points.csv is missing from " + dir.string(); return false; }
	std::string line;
	std::getline(pc, line);
	std::vector<std::string> head = splitCsv(line);
	auto at = [&](const std::vector<std::string>& f, const char* name) -> std::string {
		for (size_t i = 0; i < head.size() && i < f.size(); i++) if (head[i] == name) return f[i];
		return "";
	};
	auto num = [](const std::string& s, double d) { if (s.empty()) return d; try { return std::stod(s); } catch (...) { return d; } };
	while (std::getline(pc, line)) {
		if (line.empty() || line == "\r") continue;
		std::vector<std::string> f = splitCsv(line);
		Point p;
		p.n = int(num(at(f, "n"), 0));
		p.passed = at(f, "passed");
		p.l2r = at(f, "direction") != "R2L";
		p.compass = at(f, "compass");
		p.wrongWay = at(f, "wrongWay") == "1";
		p.zoneSeconds = num(at(f, "zoneSeconds"), 0);
		p.speed = int(num(at(f, "speedMph"), 0));
		p.trend = at(f, "trend");
		p.entryMph = int(num(at(f, "entryMph"), 0));
		p.exitMph = int(num(at(f, "exitMph"), 0));
		p.accel = num(at(f, "accelMphPerSec"), 0);
		p.mphIn20ft = int(num(at(f, "mphIn20ft"), 0));
		p.lengthFt = num(at(f, "lengthFt"), 0);
		p.flagged = at(f, "flagged") == "1";
		p.ownDrive = int(num(at(f, "ownDrive"), 0));
		p.run = at(f, "run");
		p.frame = int(num(at(f, "frame"), 0));
		out.points.push_back(p);
	}
	std::ifstream st(dir / "status.json", std::ios::binary);
	if (st) {
		json s = json::parse(st, nullptr, false);
		if (s.is_object() && s.contains("checks") && s["checks"].is_array())
			for (const json& c : s["checks"]) if (c.is_object()) { try { out.checks.push_back(checkFrom(c)); } catch (const json::exception&) {} }
	}
	return true;
}

std::vector<Info> list(const std::string& dataDir) {
	std::vector<Info> out;
	std::error_code ec;
	for (auto it = fs::directory_iterator(fs::path(dataDir) / "Published", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
		if (!it->is_directory(ec) || it->path().extension() == ".partial") continue;
		Info d;
		std::string err;
		if (read(it->path().string(), d, err)) out.push_back(d);
	}
	std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.published > b.published; });
	return out;
}

bool verify(const std::string& dir, std::vector<std::string>& changed) {
	changed.clear();
	std::ifstream in(fs::path(dir) / "SHA256SUMS", std::ios::binary);
	if (!in) return false;
	for (std::string line; std::getline(in, line);) {
		if (line.size() < 67) continue;
		std::string want = line.substr(0, 64), file = line.substr(66);
		if (!file.empty() && file.back() == '\r') file.pop_back();
		if (sha256::hexOfFile((fs::path(dir) / fs::path(file)).string()) != want) changed.push_back(file);
	}
	return true;
}

Check evaluate(const Info& in, const Calibration& newer, const std::string& now, std::vector<double>* afterOut) {
	Check c;
	c.when = now;
	c.cal = newer;
	bool has[2] = { false, false };
	for (const Point& p : in.points) if (traffic(p)) has[p.l2r ? 0 : 1] = true;
	for (int d = 0; d < 2; d++)
		if (has[d]) c.test[d] = equivalence(in.cal.zoneFt[d], in.cal.sigma[d], newer.zoneFt[d], newer.sigma[d]);
	c.verdict = std::max(c.test[0].verdict, c.test[1].verdict);   // the enum runs None < Accurate < Review < Tainted
	std::vector<Point> moved = in.points;
	std::vector<double> after;
	double sum = 0;
	int n = 0;
	for (size_t i = 0; i < moved.size(); i++) {
		Point& p = moved[i];
		int d = p.l2r ? 0 : 1;
		double a = p.speed;
		if (newer.zoneFt[d] > 0)
			a = p.zoneSeconds > 0 ? newer.zoneFt[d] / p.zoneSeconds / kFtPerSecPerMph
				: in.cal.zoneFt[d] > 0 ? p.speed * newer.zoneFt[d] / in.cal.zoneFt[d] : p.speed;
		after.push_back(a);
		if (traffic(p)) {
			double diff = a - p.speed;
			sum += diff;
			n++;
			if (std::fabs(diff) > std::fabs(c.maxDiff)) c.maxDiff = diff;
		}
		p.speed = int(a + 0.5);
	}
	c.meanDiff = n ? sum / n : 0;
	c.before = summarize(in.points, in.speedLimit, in.wayOver);
	c.after = summarize(moved, in.speedLimit, in.wayOver);
	c.file = "checks/" + isoStamp(now) + ".csv";
	if (afterOut) *afterOut = after;
	return c;
}

bool check(Info& in, const Calibration& newer, const std::string& now, Check& out, std::string& error) {
	std::vector<double> after;
	out = evaluate(in, newer, now, &after);
	fs::path dir(in.dir), cs = dir / fs::path(out.file);
	std::error_code ec;
	fs::create_directories(cs.parent_path(), ec);
	std::string csv = "n,direction,before,after,difference,includedInNumbers\n";
	for (size_t i = 0; i < in.points.size(); i++) {
		const Point& p = in.points[i];
		csv += std::to_string(p.n) + "," + (p.l2r ? "L2R" : "R2L") + "," + std::to_string(p.speed) + "," + fixed(after[i], 2) + ","
			+ fixed(after[i] - p.speed, 2) + "," + (traffic(p) ? "1" : "0") + "\n";
	}
	if (!writeText(cs, csv)) { error = "Couldn't write " + cs.string(); return false; }
	in.checks.push_back(out);
	fs::path tmp = dir / "status.json.tmp";
	if (!writeText(tmp, statusJson(in))) { in.checks.pop_back(); error = "Couldn't write status.json in " + dir.string(); return false; }
	fs::rename(tmp, dir / "status.json", ec);
	if (ec) { in.checks.pop_back(); error = "Couldn't write status.json: " + ec.message(); return false; }
	// review.html shows the new badge.  It isn't in SHA256SUMS, so redrawing it leaves the checksums valid.
	archive::Info a;
	std::string err;
	if (archive::read(in.dir, a, err)) {
		a.dataset = coverJson(in);
		writeText(dir / "review.html", archive::reviewHtml(a));
	}
	return true;
}

std::string accuracyWords(const Calibration& c, int limit) {
	double band = std::max(c.band(0), c.band(1));
	double mph = band * limit;
	std::string pct = fixed(band * 100, band * 100 < 9.95 ? 1 : 0);
	if (pct.size() > 2 && pct.compare(pct.size() - 2, 2, ".0") == 0) pct.resize(pct.size() - 2);
	return "within \xC2\xB1" + fixed(mph, mph < 9.95 ? 1 : 0) + " mph at " + std::to_string(limit) + " mph (\xC2\xB1" + pct + "%)";
}

std::string sentence(const Info& in) {
	std::string promise = "\xC2\xB1" + fixed(std::max(in.cal.band(0), in.cal.band(1)) * 100, 1) + "%";
	if (in.checks.empty())
		return "Published " + shortDay(in.published) + ". Speeds are accurate to " + accuracyWords(in.cal, in.speedLimit)
			+ ", with 95% confidence. Not re-checked since.";
	const Check& c = in.checks.back();
	// The direction that decided the verdict says how far off, and which way.
	const Test& t = c.test[c.test[0].verdict >= c.test[1].verdict ? 0 : 1];
	std::string off = fixed(std::fabs(t.delta) * 100, 1) + "%";
	std::string way = t.delta < 0 ? "high" : "low";   // a shorter zone than published means the published speeds were too high
	std::string when = shortDay(c.when);
	switch (c.verdict) {
	case Verdict::Accurate:
		return "Re-checked " + when + " against a newer calibration: still within the " + promise + " promised (" + off + " different).";
	case Verdict::Review:
		return "A newer calibration (" + when + ") reads " + off + " different. That may or may not be within the " + promise
			+ " promised; one more check drive will settle it.";
	case Verdict::Tainted:
		return "A newer calibration (" + when + ") shows these speeds read about " + off + " " + way + ", more than the " + promise
			+ " promised. Use the corrected speeds.";
	default:
		return "Published " + shortDay(in.published) + ".";
	}
}

}  // namespace dataset
