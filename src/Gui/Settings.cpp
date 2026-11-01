#include "Settings.h"
#include "UserPaths.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
	for (char& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

std::string trim(const std::string& s) {
	size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Replaces `path` with `text` without ever leaving a half-written file: write a sibling temp file, then rename
// it over the target (rename replaces atomically on POSIX; on Windows std::filesystem uses MoveFileEx with
// REPLACE_EXISTING).  A crash or power cut mid-save leaves either the old file or the new one, never neither.
bool writeAtomic(const fs::path& path, const std::string& text) {
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	fs::path tmp = path;
	tmp += ".tmp";
	{
		std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << text;
		out.flush();
		if (!out) { out.close(); fs::remove(tmp, ec); return false; }
	}
	fs::rename(tmp, path, ec);
	if (ec) { fs::remove(tmp, ec); return false; }
	return true;
}

std::string num(double v) {
	std::ostringstream o;
	o << v;
	return o.str();
}

// Every key the tracker reads, in the order a fresh file lists them, with how to print its current value.
const std::vector<std::pair<std::string, std::function<std::string(const Globals&)>>>& table() {
	static const std::vector<std::pair<std::string, std::function<std::string(const Globals&)>>> t = {
		{ "dataPathPrefix", [](const Globals& g) { return g.dataPathPrefix; } },
		{ "L2RDirection", [](const Globals& g) { return "\"" + g.L2RDirection + "\""; } },
		{ "R2LDirection", [](const Globals& g) { return "\"" + g.R2LDirection + "\""; } },
		{ "LegalDirection", [](const Globals& g) { return g.LegalDirection; } },
		{ "obstruction", [](const Globals& g) { return "[" + std::to_string(g.obstruction[0]) + "," + std::to_string(g.obstruction[1]) + "]"; } },
		{ "AnalysisBoxLeft", [](const Globals& g) { return std::to_string(g.AnalysisBoxLeft); } },
		{ "AnalysisBoxTop", [](const Globals& g) { return std::to_string(g.AnalysisBoxTop); } },
		{ "AnalysisBoxWidth", [](const Globals& g) { return std::to_string(g.AnalysisBoxWidth); } },
		{ "AnalysisBoxHeight", [](const Globals& g) { return std::to_string(g.AnalysisBoxHeight); } },
		{ "speedLineLeft", [](const Globals& g) { return std::to_string(g.speedLineLeft); } },
		{ "speedLineRight", [](const Globals& g) { return std::to_string(g.speedLineRight); } },
		{ "maxL2RDistOnEntry", [](const Globals& g) { return std::to_string(g.maxL2RDistOnEntry); } },
		{ "maxR2LDistOnEntry", [](const Globals& g) { return std::to_string(g.maxR2LDistOnEntry); } },
		{ "entryLookBack", [](const Globals& g) { return std::to_string(g.entryLookBack); } },
		{ "obstruction_extent", [](const Globals& g) { return std::to_string(g.obstruction_extent); } },
		{ "largeVehicleArea", [](const Globals& g) { return std::to_string(g.largeVehicleArea); } },
		{ "CalibrationFramesL2R", [](const Globals& g) { return std::to_string(g.CalibrationFramesL2R); } },
		{ "CalibrationFramesR2L", [](const Globals& g) { return std::to_string(g.CalibrationFramesR2L); } },
		{ "CalibrationSpeed", [](const Globals& g) { return num(g.CalibrationSpeed); } },
		{ "CalibrationFPS", [](const Globals& g) { return num(g.CalibrationFPS); } },
		{ "SENSITIVITY_VALUE", [](const Globals& g) { return std::to_string(g.SENSITIVITY_VALUE); } },
		{ "BLUR_SIZE", [](const Globals& g) { return std::to_string(g.BLUR_SIZE); } },
		{ "SLOP", [](const Globals& g) { return std::to_string(g.SLOP); } },
		{ "R2LStreetY", [](const Globals& g) { return std::to_string(g.R2LStreetY); } },
		{ "L2RStreetY", [](const Globals& g) { return std::to_string(g.L2RStreetY); } },
		{ "R2LStreetYRight", [](const Globals& g) { return std::to_string(g.R2LStreetYRight); } },
		{ "L2RStreetYRight", [](const Globals& g) { return std::to_string(g.L2RStreetYRight); } },
		{ "streetEdge", [](const Globals& g) { return "[" + std::to_string(g.streetEdge[0]) + "," + std::to_string(g.streetEdge[1]) + "]"; } },
		{ "nextHeight", [](const Globals& g) { return std::to_string(g.nextHeight); } },
		{ "minLoggedSpeed", [](const Globals& g) { return std::to_string(g.minLoggedSpeed); } },
		{ "subFrameTiming", [](const Globals& g) { return std::to_string(g.subFrameTiming); } },
		{ "levelDegrees", [](const Globals& g) { return num(g.levelDegrees); } },
	};
	return t;
}

// readConfig() prints to cout/cerr; capture it so the GUI can show why a file was rejected.
struct Capture {
	std::ostringstream buf;
	std::streambuf *out, *err;
	Capture() : out(std::cout.rdbuf(buf.rdbuf())), err(std::cerr.rdbuf(buf.rdbuf())) {}
	~Capture() { std::cout.rdbuf(out); std::cerr.rdbuf(err); }
	std::vector<std::string> lines() const {
		std::vector<std::string> v;
		std::istringstream in(buf.str());
		for (std::string l; std::getline(in, l);) if (!l.empty()) v.push_back(l);
		return v;
	}
};

}  // namespace

const std::vector<std::string>& configKeys() {
	static std::vector<std::string> keys = [] {
		std::vector<std::string> k;
		for (auto& e : table()) k.push_back(e.first);
		return k;
	}();
	return keys;
}

std::string configValue(const Globals& g, const std::string& key) {
	for (auto& e : table())
		if (lower(e.first) == lower(key)) return e.second(g);
	return "";
}

bool ConfigDoc::load(const std::string& path) {
	error.clear();
	Globals fresh;
	fresh.obstruction[0] = fresh.obstruction[1] = 0;
	bool ok;
	{
		Capture cap;
		ok = fresh.readConfig(path);
		log = cap.lines();
	}
	if (!ok) {
		error = "The tracker can't use " + path + ":";
		for (auto& l : log)
			if (l.find(" = ") == std::string::npos) error += "\n" + l;   // skip the "key = value" echo lines
		return false;
	}
	// Globals can't be assigned (const member); rebuild in place from the parsed copy's values.
	g.~Globals();
	new (&g) Globals(fresh);
	path_ = path;
	lines_.clear();
	std::ifstream in(path);
	for (std::string l; std::getline(in, l);) {
		if (!l.empty() && l.back() == '\r') l.pop_back();
		lines_.push_back(l);
	}
	markClean();
	return true;
}

std::string ConfigDoc::render() const {
	// Replace values in place; keep comments after '#'; append keys the file doesn't mention.
	std::map<std::string, bool> seen;
	std::string out;
	for (const std::string& line : lines_) {
		std::string body = line.substr(0, line.find('#'));
		std::string comment = line.find('#') != std::string::npos ? line.substr(line.find('#')) : "";
		size_t eq = body.find('=');
		std::string key = eq == std::string::npos ? "" : trim(body.substr(0, eq));
		std::string value = key.empty() ? "" : configValue(g, key);
		if (key.empty() || value.empty() && lower(key) != "datapathprefix") { out += line + "\n"; continue; }
		seen[lower(key)] = true;
		std::string lhs = body.substr(0, eq);
		std::string rendered = lhs + "= " + value;
		if (!comment.empty()) {
			// Keep the comment column where it was if the new value still fits before it.
			size_t col = line.find('#');
			rendered += rendered.size() + 1 < col ? std::string(col - rendered.size(), ' ') : std::string("   ");
			rendered += comment;
		}
		out += rendered + "\n";
	}
	bool header = false;
	for (auto& e : table()) {
		if (seen[lower(e.first)]) continue;
		if (!header && !lines_.empty()) { out += "\n# Added by VST Studio\n"; header = true; }
		out += e.first + " = " + e.second(g) + "\n";
	}
	return out;
}

bool ConfigDoc::save() {
	if (path_.empty()) { error = "No config file to save to"; return false; }
	std::error_code ec;
	fs::create_directories(fs::path(path_).parent_path(), ec);
	std::string text = render();
	// The first save of a file in this session copies the original to <file>.bak, so "Undo changes" can get
	// back to how things were when Studio opened it, however many autosaves happened since.
	if (backedUp_ != path_ && fs::exists(path_, ec)) {
		fs::copy_file(path_, backupPath(), fs::copy_options::overwrite_existing, ec);
		if (!ec) backedUp_ = path_;
	}
	if (!writeAtomic(path_, text)) { error = "Can't write " + path_; return false; }
	// Re-read the lines so a second save is relative to what's on disk now.
	lines_.clear();
	std::istringstream in(text);
	for (std::string l; std::getline(in, l);) lines_.push_back(l);
	markClean();
	return true;
}

std::string ConfigDoc::backupPath() const { return path_.empty() ? std::string() : path_ + ".bak"; }

bool ConfigDoc::hasBackup() const {
	std::error_code ec;
	return backedUp_ == path_ && !path_.empty() && fs::exists(backupPath(), ec);
}

bool ConfigDoc::restoreBackup() {
	// Put the session's original back on disk and reload it; the .bak stays, so this can be repeated.
	if (!hasBackup()) { error = "Nothing to undo yet"; return false; }
	std::ifstream in(backupPath(), std::ios::binary);
	std::stringstream text;
	text << in.rdbuf();
	if (!writeAtomic(path_, text.str())) { error = "Can't write " + path_; return false; }
	std::string keep = path_;
	return load(keep);
}

bool ConfigDoc::saveAs(const std::string& path) {
	path_ = path;
	return save();
}

std::string ConfigDoc::writeRunCopy(const std::string& dir) const {
	std::error_code ec;
	fs::create_directories(dir, ec);
	std::string p = (fs::path(dir) / "last-run.cfg").string();
	std::ofstream outFile(p, std::ios::binary);
	outFile << "# Settings VST Studio used for its most recent run (unsaved edits included).\n" << render();
	return outFile ? p : std::string();
}

bool ConfigDoc::dirty() const {
	std::string now;
	for (auto& e : table()) now += e.second(g) + "\n";
	return now != cleanSnapshot_;
}

void ConfigDoc::markClean() {
	cleanSnapshot_.clear();
	for (auto& e : table()) cleanSnapshot_ += e.second(g) + "\n";
}

// ---- AppPrefs ------------------------------------------------------------------------------------------

std::string AppPrefs::file() { return (userpaths::configDir() / "vst-studio.ini").string(); }

void AppPrefs::load() {
	std::ifstream in(file());
	std::map<std::string, std::string> kv;
	for (std::string l; std::getline(in, l);) {
		size_t eq = l.find('=');
		if (eq == std::string::npos) continue;
		std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
		if (k == "recent") recent.push_back(v);
		else kv[k] = v;
	}
	auto str = [&](const char* k, std::string& f) { if (kv.count(k)) f = kv[k]; };
	auto i = [&](const char* k, int& f) { try { if (kv.count(k)) f = std::stoi(kv[k]); } catch (...) {} };
	auto d = [&](const char* k, double& f) { try { if (kv.count(k)) f = std::stod(kv[k]); } catch (...) {} };
	auto b = [&](const char* k, bool& f) { if (kv.count(k)) f = kv[k] == "1" || kv[k] == "true"; };
	str("config", configPath); str("source", lastSource); str("site", siteName); str("remoteToken", remoteToken);
	str("remoteAddress", remoteAddress);
	str("zonesFor", zonesFor); str("calibratedFor", calibratedFor);
	str("planNote", planNote); str("planCalSig", planCalSig); i("planSource", planSource); d("planCarLengths", planCarLengths);
	i("speedLimit", speedLimit); i("egregious", egregious); i("hlMin", hlMin); i("hlMax", hlMax); i("hlArea", hlArea);
	i("remotePort", remotePort); i("keepVideoGB", keepVideoGB); i("planPreset", planPreset);
	b("realtime", realtime); b("highlights", highlights); b("trace", trace); b("showMotion", showMotion);
	b("guideDone", guideDone); b("remote", remoteEnabled);
	d("planHfov", planHfov); d("planWidth", planWidth); d("planFps", planFps); d("planShutter", planShutter);
	d("planDist", planDist); d("planClear", planClear); d("planCalib", planCalib); d("planCarLen", planCarLen);
	d("siteLat", siteLat); d("siteLon", siteLon); d("calcHeightFt", calcHeightFt); d("calcYawDeg", calcYawDeg);
	if (remoteToken.empty()) remoteToken = newToken();
}

std::string AppPrefs::newToken() {
	std::random_device rd;
	const char* alphabet = "abcdefghijkmnpqrstuvwxyz23456789";
	std::string t;
	for (int n = 0; n < 24; n++) t += alphabet[rd() % 32];
	return t;
}

void AppPrefs::save() const {
	std::ostringstream out;
	out << "# VST Studio preferences\n"
		<< "config = " << configPath << "\nsource = " << lastSource << "\nsite = " << siteName << "\n"
		<< "speedLimit = " << speedLimit << "\negregious = " << egregious << "\n"
		<< "realtime = " << realtime << "\nhighlights = " << highlights << "\nhlMin = " << hlMin << "\nhlMax = " << hlMax
		<< "\nhlArea = " << hlArea << "\ntrace = " << trace << "\nshowMotion = " << showMotion << "\nguideDone = " << guideDone
		<< "\nremote = " << remoteEnabled << "\nremotePort = " << remotePort << "\nremoteToken = " << remoteToken << "\nremoteAddress = " << remoteAddress << "\nkeepVideoGB = " << keepVideoGB
		<< "\nplanPreset = " << planPreset << "\nplanHfov = " << planHfov << "\nplanWidth = " << planWidth << "\nplanFps = " << planFps
		<< "\nplanShutter = " << planShutter << "\nplanDist = " << planDist << "\nplanClear = " << planClear
		<< "\nplanCalib = " << planCalib << "\nplanCarLen = " << planCarLen << "\nplanSource = " << planSource
		<< "\nplanNote = " << planNote << "\nplanCalSig = " << planCalSig << "\nplanCarLengths = " << planCarLengths
		<< "\ncalcHeightFt = " << calcHeightFt << "\ncalcYawDeg = " << calcYawDeg << "\nzonesFor = " << zonesFor
		<< "\nsiteLat = " << std::setprecision(10) << siteLat << "\nsiteLon = " << siteLon << std::setprecision(6)
		<< "\ncalibratedFor = " << calibratedFor << "\n";
	for (auto& r : recent) out << "recent = " << r << "\n";
	writeAtomic(file(), out.str());   // holds the pairing token: never leave it truncated
}

void AppPrefs::addRecent(const std::string& path) {
	recent.erase(std::remove(recent.begin(), recent.end(), path), recent.end());
	recent.insert(recent.begin(), path);
	if (recent.size() > 8) recent.resize(8);
}
