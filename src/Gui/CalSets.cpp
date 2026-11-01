#include "CalSets.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace calsets {

namespace {
constexpr double kFtPerSecPerMph = 5280.0 / 3600.0;
constexpr double kProjectFeet = 20.0;   // SpeedTrend.h kProjectFeet: MphIn20ft looks this far past the exit line

bool sameNumber(double a, double b, double rel) { return std::fabs(a - b) <= rel * std::max({ std::fabs(a), std::fabs(b), 1e-9 }); }

// run_<stem>.json beside stats_<stem>.csv (vst::manifestPathFor, without linking the engine).
fs::path manifestOf(const fs::path& csv) {
	std::string stem = csv.stem().string();
	stem = stem.compare(0, 6, "stats_") == 0 ? "run_" + stem.substr(6) : "run_" + stem;
	return csv.parent_path() / (stem + ".json");
}

json viewJson(const View& v) {
	return { { "speedLines", { v.lineLeft, v.lineRight } }, { "streetEdge", { v.edge[0], v.edge[1] } }, { "levelDegrees", v.level },
		{ "analysisBox", { v.box[0], v.box[1], v.box[2], v.box[3] } } };
}
View viewFrom(const json& j) {
	View v;
	if (!j.is_object()) return v;
	const json& l = j.value("speedLines", json::array());
	if (l.is_array() && l.size() == 2) { v.lineLeft = l[0].get<int>(); v.lineRight = l[1].get<int>(); }
	const json& e = j.value("streetEdge", json::array());
	if (e.is_array() && e.size() == 2) { v.edge[0] = e[0].get<int>(); v.edge[1] = e[1].get<int>(); }
	v.level = j.value("levelDegrees", 0.0);
	const json& b = j.value("analysisBox", json::array());
	if (b.is_array() && b.size() == 4) for (int i = 0; i < 4; i++) v.box[i] = b[size_t(i)].get<int>();
	return v;
}
json scaleJson(const Scale& s) { return { { "L2R", s.l2r }, { "R2L", s.r2l } }; }
Scale scaleFrom(const json& j) {
	Scale s;
	if (j.is_object()) { s.l2r = j.value("L2R", 0.0); s.r2l = j.value("R2L", 0.0); }
	return s;
}
json strings(const std::vector<std::string>& v) { json a = json::array(); for (const auto& s : v) a.push_back(s); return a; }
std::vector<std::string> stringsFrom(const json& j) {
	std::vector<std::string> v;
	if (j.is_array()) for (const json& s : j) if (s.is_string()) v.push_back(s.get<std::string>());
	return v;
}

// Earlier first; runs with no start time go after timed ones, by file name (the tracker names them by date and time).
bool filedBefore(const RunInfo& a, const RunInfo& b) {
	if (a.started.empty() != b.started.empty()) return !a.started.empty();
	if (a.started != b.started) return a.started < b.started;
	return a.csv < b.csv;
}
}  // namespace

bool View::same(const View& o) const {
	if (lineLeft != o.lineLeft || lineRight != o.lineRight || edge[0] != o.edge[0] || edge[1] != o.edge[1]) return false;
	if (std::fabs(level - o.level) > 0.01) return false;
	if (hasBox() && o.hasBox()) for (int i = 0; i < 4; i++) if (box[i] != o.box[i]) return false;
	return true;
}

bool Scale::same(const Scale& o) const { return sameNumber(l2r, o.l2r, 0.0005) && sameNumber(r2l, o.r2l, 0.0005); }

double zoneFeet(double calFrames, double calSpeed, double calFps) {
	return calFps > 0 && calFrames > 0 ? calSpeed * kFtPerSecPerMph * calFrames / calFps : 0;
}

RunInfo readRun(const std::string& csvPath) {
	RunInfo r;
	fs::path csv(csvPath);
	r.csv = csv.filename().string();
	r.path = csvPath;
	std::ifstream in(manifestOf(csv), std::ios::binary);
	if (!in) return r;
	json j = json::parse(in, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return r;
	if (j.contains("started") && j["started"].is_string()) r.started = j["started"].get<std::string>();
	const json& c = j.value("config", json::object());
	if (!c.is_object() || !c.contains("CalibrationFramesL2R")) return r;
	try {
		double speed = c.value("CalibrationSpeed", 25.0), fps = c.value("CalibrationFPS", 30.0);
		r.scale.l2r = zoneFeet(c.value("CalibrationFramesL2R", 0.0), speed, fps);
		r.scale.r2l = zoneFeet(c.value("CalibrationFramesR2L", 0.0), speed, fps);
		r.view.lineLeft = c.value("speedLineLeft", 0);
		r.view.lineRight = c.value("speedLineRight", 0);
		r.view.level = c.value("levelDegrees", 0.0);
		const json& e = c.value("streetEdge", json::array());
		if (e.is_array() && e.size() == 2) { r.view.edge[0] = e[0].get<int>(); r.view.edge[1] = e[1].get<int>(); }
		const json& b = c.value("analysisBox", json::array());
		if (b.is_array() && b.size() == 4) for (int i = 0; i < 4; i++) r.view.box[i] = b[size_t(i)].get<int>();
	}
	catch (const json::exception&) { return r; }   // a hand-edited manifest with a wrong type: calibration unknown
	r.known = r.scale.valid();
	return r;
}

bool Set::has(const std::string& csvName) const { return std::find(runs.begin(), runs.end(), csvName) != runs.end(); }

Set* Store::find(int id) { for (Set& s : sets) if (s.id == id) return &s; return nullptr; }
const Set* Store::find(int id) const { for (const Set& s : sets) if (s.id == id) return &s; return nullptr; }
const Set* Store::ofRun(const std::string& csvName) const {
	std::string name = fs::path(csvName).filename().string();
	for (const Set& s : sets) if (s.has(name)) return &s;
	return nullptr;
}

std::string storePath(const std::string& statsDir) { return (fs::path(statsDir) / "calibration-sets.json").string(); }

Store load(const std::string& statsDir) {
	Store st;
	std::ifstream in(fs::path(storePath(statsDir)), std::ios::binary);
	if (!in) return st;
	json j = json::parse(in, nullptr, false);
	if (j.is_discarded() || !j.is_object()) return st;
	try {
		st.nextId = j.value("nextId", 1);
		for (const json& s : j.value("sets", json::array())) {
			Set set;
			set.id = s.value("id", 0);
			set.known = s.value("known", true);
			set.view = viewFrom(s.value("view", json::object()));
			set.state = s.value("state", std::string("open")) == "locked" ? State::Locked : State::Open;
			set.scale = scaleFrom(s.value("zoneFt", json::object()));
			set.sigma = scaleFrom(s.value("sigma", json::object()));
			set.lockedAt = s.value("lockedAt", std::string());
			set.lockReason = s.value("lockReason", std::string());
			set.first = s.value("first", std::string());
			set.last = s.value("last", std::string());
			set.runs = stringsFrom(s.value("runs", json::array()));
			set.published = stringsFrom(s.value("published", json::array()));
			if (set.id <= 0) continue;
			st.nextId = std::max(st.nextId, set.id + 1);
			st.sets.push_back(set);
		}
	}
	catch (const json::exception&) { return Store(); }
	return st;
}

std::string dump(const Store& st) {
	json sets = json::array();
	for (const Set& s : st.sets) {
		json o = { { "id", s.id }, { "known", s.known }, { "state", s.locked() ? "locked" : "open" }, { "view", viewJson(s.view) },
			{ "zoneFt", scaleJson(s.scale) }, { "sigma", scaleJson(s.sigma) }, { "first", s.first }, { "last", s.last },
			{ "runs", strings(s.runs) }, { "published", strings(s.published) } };
		if (s.locked()) { o["lockedAt"] = s.lockedAt; o["lockReason"] = s.lockReason; }
		sets.push_back(o);
	}
	json j = { { "format", 1 }, { "nextId", st.nextId }, { "sets", sets } };
	return j.dump(1, '\t') + "\n";
}

bool save(const std::string& statsDir, const Store& st, std::string& error) {
	std::error_code ec;
	fs::create_directories(statsDir, ec);
	fs::path path = storePath(statsDir), tmp = path.string() + ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		out << dump(st);
		if (!out) { error = "Couldn't write " + tmp.string(); return false; }
	}
	fs::rename(tmp, path, ec);   // replaces the old file in one step
	if (ec) { error = "Couldn't write " + path.string() + ": " + ec.message(); fs::remove(tmp, ec); return false; }
	return true;
}

bool sync(Store& st, std::vector<RunInfo> runs, const View& current, const Scale& currentScale, const Scale& currentSigma,
	const std::string& now) {
	bool changed = false;
	// Runs that are gone leave their set; a set left empty goes (its published folders stand on their own).
	std::vector<std::string> present;
	for (const RunInfo& r : runs) present.push_back(r.csv);
	for (Set& s : st.sets) {
		size_t before = s.runs.size();
		s.runs.erase(std::remove_if(s.runs.begin(), s.runs.end(),
			[&](const std::string& c) { return std::find(present.begin(), present.end(), c) == present.end(); }), s.runs.end());
		changed |= s.runs.size() != before;
	}
	size_t setsBefore = st.sets.size();
	st.sets.erase(std::remove_if(st.sets.begin(), st.sets.end(), [](const Set& s) { return s.runs.empty(); }), st.sets.end());
	changed |= st.sets.size() != setsBefore;

	std::sort(runs.begin(), runs.end(), filedBefore);
	for (const RunInfo& r : runs) {
		if (st.ofRun(r.csv)) continue;
		changed = true;
		Set* into = nullptr;
		if (!r.known) {
			for (Set& s : st.sets) if (!s.known) into = &s;
			if (!into) {
				Set s;
				s.id = st.nextId++;
				s.known = false;
				s.state = State::Locked;   // nothing to re-adjust it with
				s.lockReason = "unknown";
				st.sets.push_back(s);
				into = &st.sets.back();
			}
		}
		else {
			for (Set& s : st.sets) if (s.known && !s.locked() && s.view.same(r.view)) into = &s;
			if (!into) {
				// A new view: every other open set stops collecting, keeping the calibration it last had.
				for (Set& s : st.sets)
					if (s.known && !s.locked()) { s.state = State::Locked; s.lockReason = "view"; s.lockedAt = !r.started.empty() ? r.started : now; }
				Set s;
				s.id = st.nextId++;
				s.view = r.view;
				st.sets.push_back(s);
				into = &st.sets.back();
			}
			// An open set takes the calibration of its newest run (today's replaces it below when it's on today's view).
			into->scale = r.scale;
			if (r.view.hasBox() && !into->view.hasBox()) for (int i = 0; i < 4; i++) into->view.box[i] = r.view.box[i];
		}
		into->runs.push_back(r.csv);
		if (into->first.empty() || (!r.started.empty() && r.started < into->first)) into->first = r.started;
		if (!r.started.empty() && r.started > into->last) into->last = r.started;
	}
	// The open set on today's view follows today's calibration.
	if (currentScale.valid())
		for (Set& s : st.sets)
			if (s.known && !s.locked() && s.view.same(current) && (!s.scale.same(currentScale) || !s.sigma.same(currentSigma))) {
				s.scale = currentScale;
				s.sigma = currentSigma;
				changed = true;
			}
	return changed;
}

Scale shownScale(const Set& s, const View& current, const Scale& currentScale) {
	if (s.known && !s.locked() && s.view.same(current) && currentScale.valid()) return currentScale;
	return s.scale;
}

bool newerCalibration(const Set& s, const View& current, const Scale& currentScale) {
	return s.known && s.locked() && s.view.same(current) && currentScale.valid() && s.scale.valid() && !s.scale.same(currentScale);
}

bool lock(Store& st, int id, const View& current, const Scale& currentScale, const Scale& currentSigma, const std::string& now) {
	Set* s = st.find(id);
	if (!s || s->locked() || !s->known) return false;
	if (s->view.same(current) && currentScale.valid()) { s->scale = currentScale; s->sigma = currentSigma; }
	s->state = State::Locked;
	s->lockReason = "manual";
	s->lockedAt = now;
	return true;
}

bool canUnlock(const Store& st, int id, const View& current) {
	const Set* s = st.find(id);
	if (!s || !s->locked() || !s->known || !s->view.same(current)) return false;
	for (const Set& o : st.sets)
		if (o.id != id && o.known && !o.locked() && o.view.same(current)) return false;
	return true;
}

bool unlock(Store& st, int id, const View& current) {
	if (!canUnlock(st, id, current)) return false;
	Set* s = st.find(id);
	s->state = State::Open;
	s->lockReason.clear();
	s->lockedAt.clear();
	return true;
}

bool accept(Store& st, int id, const View& current, const Scale& currentScale, const Scale& currentSigma) {
	Set* s = st.find(id);
	if (!s || !newerCalibration(*s, current, currentScale)) return false;
	s->scale = currentScale;
	s->sigma = currentSigma;
	return true;
}

Retimed retime(const Measured& m, double thenFt, double zoneFt) {
	Retimed r;
	r.speed = m.speed;
	r.exact = m.speed;
	r.entryMph = m.entryMph;
	r.exitMph = m.exitMph;
	r.mphIn20ft = m.mphIn20ft;
	r.accel = m.accel;
	if (!(zoneFt > 0)) return r;
	if (m.zoneSeconds > 0) {
		r.exact = zoneFt / m.zoneSeconds / kFtPerSecPerMph;
		// The engine's speed at its own zone feet tells what the trend columns were scaled by.
		r.factor = thenFt > 0 ? zoneFt / thenFt : (m.speed > 0 ? r.exact / m.speed : 1);
	}
	else {
		if (!(thenFt > 0)) return r;
		r.factor = zoneFt / thenFt;
		r.exact = m.speed * r.factor;
	}
	r.speed = int(r.exact + 0.5);
	if (m.trend) {
		// Speeds and acceleration scale with the zone; the projection is carried kProjectFeet of real street again,
		// sqrt(v^2 + 2 a d) (SpeedTrend.h), and stays the exit speed for a car the tracker called steady.
		double entry = m.entryMph * r.factor, exit = m.exitMph * r.factor;
		r.accel = std::round(m.accel * r.factor * 10) / 10;
		r.entryMph = int(entry + 0.5);
		r.exitMph = int(exit + 0.5);
		if (m.mphIn20ft == m.exitMph) r.mphIn20ft = r.exitMph;
		else {
			double v = exit * kFtPerSecPerMph, a = m.accel * r.factor * kFtPerSecPerMph, v2 = v * v + 2 * a * kProjectFeet;
			r.mphIn20ft = v2 > 0 ? int(std::sqrt(v2) / kFtPerSecPerMph + 0.5) : 0;
		}
	}
	r.adjusted = r.speed != m.speed;
	return r;
}

std::string shortDate(const std::string& iso) {
	static const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	if (iso.size() < 10 || iso[4] != '-' || iso[7] != '-') return "";
	int m = std::atoi(iso.substr(5, 2).c_str()), d = std::atoi(iso.substr(8, 2).c_str());
	if (m < 1 || m > 12 || d < 1) return "";
	return std::string(months[m - 1]) + " " + std::to_string(d);
}

std::string describe(const Set& s, const Scale& shown) {
	std::ostringstream o;
	o.imbue(std::locale::classic());
	o << "Set " << s.id;
	std::string a = shortDate(s.first), b = shortDate(s.last);
	if (!a.empty()) o << " \xC2\xB7 " << a << (b.empty() || b == a ? "" : " to " + b);
	o << " \xC2\xB7 " << s.runs.size() << (s.runs.size() == 1 ? " run" : " runs");
	if (!s.known) { o << " \xC2\xB7 calibration not on record"; return o.str(); }
	if (shown.valid()) {
		o.setf(std::ios::fixed);
		o.precision(1);
		// Each lane is its own distance from the camera, so the two directions' zones can differ.
		if (std::fabs(shown.l2r - shown.r2l) < 0.05) o << " \xC2\xB7 zone " << shown.l2r << " ft";
		else o << " \xC2\xB7 zone " << shown.l2r << " / " << shown.r2l << " ft";
	}
	if (!s.published.empty()) o << " \xC2\xB7 published";
	else if (s.locked()) o << " \xC2\xB7 locked";
	return o.str();
}

}  // namespace calsets
