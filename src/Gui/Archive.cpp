// Archived datasets: see Archive.h for the folder layout and the write -> move -> read -> restore flow.
#include "Archive.h"
#include "Report.h"
#include "Sha256.h"
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace archive {

// ---- small helpers -----------------------------------------------------------------------------------------

static bool writeText(const fs::path& p, const std::string& text) {
	std::ofstream out(p, std::ios::binary);
	out << text;
	return bool(out);
}

// A csv cell: quoted when it holds a comma, quote or line break (RFC 4180), so a source named "Cam, front" stays one cell.
static std::string cell(const std::string& s) {
	if (s.find_first_of(",\"\r\n") == std::string::npos) return s;
	std::string o = "\"";
	for (char c : s) { if (c == '"') o += '"'; o += c; }
	return o + '"';
}

// Copies src to dst (dst's folder must exist).  Returns false with error set.
static bool copyInto(const fs::path& src, const fs::path& dst, std::string& error) {
	std::error_code ec;
	fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
	if (ec) { error = "Couldn't copy " + src.string() + ": " + ec.message(); return false; }
	return true;
}

std::string manifestFor(const std::string& csv) {
	fs::path p(csv);
	std::string stem = p.stem().string();
	std::string name = stem.rfind("stats_", 0) == 0 ? "run_" + stem.substr(6) : "run_" + stem;
	return (p.parent_path() / (name + ".json")).string();
}

std::string highlightsFor(const std::string& csv) {
	// <data>/Stats/stats_<stem>.csv -> <data>/HiLites/Hilites_<stem>.mp4
	fs::path p(csv);
	std::string stem = p.stem().string();
	if (stem.rfind("stats_", 0) == 0) stem = stem.substr(6);
	return (p.parent_path().parent_path() / "HiLites" / ("Hilites_" + stem + ".mp4")).string();
}

std::string folderFor(const std::string& archiveRoot, const std::string& created, const std::string& name) {
	std::string day = created.size() >= 10 ? created.substr(0, 10) : "archive";
	std::string base = day + "_" + report::slug(name.empty() ? std::string("dataset") : name);
	fs::path root(archiveRoot);
	std::error_code ec;
	std::string pick = base;
	for (int n = 2; fs::exists(root / pick, ec) || fs::exists(root / (pick + ".partial"), ec); n++) pick = base + "-" + std::to_string(n);
	return (root / pick).string();
}

// ---- archive.json ------------------------------------------------------------------------------------------

static json toJson(const Info& in) {
	json vs = json::array();
	for (const Vehicle& v : in.vehicles)
		vs.push_back({ { "passed", v.passed }, { "speed", v.speed }, { "direction", v.l2r ? "L2R" : "R2L" }, { "compass", v.compass },
			{ "wrongWay", v.wrongWay }, { "frames", v.frames }, { "area", v.area }, { "frame", v.frame }, { "large", v.large },
			{ "flagged", v.flagged }, { "fps", v.fps }, { "source", v.source }, { "snapshot", v.snapshot }, { "clip", v.clip },
			{ "lengthFt", std::round(v.lengthFt * 10) / 10 }, { "trend", v.trend }, { "entryMph", v.entryMph }, { "exitMph", v.exitMph },
			{ "accel", v.accel }, { "mphIn20ft", v.mphIn20ft }, { "ownDrive", v.ownDrive }, { "drivenMph", v.drivenMph } });
	const Calibration& c = in.cal;
	return {
		{ "format", "vst-archive" }, { "version", 1 },
		{ "name", in.name }, { "created", in.created }, { "from", in.from }, { "siteName", in.siteName },
		{ "speedLimit", in.speedLimit }, { "wayOver", in.wayOver }, { "vst", in.vst }, { "configPath", in.configPath },
		{ "calibration", { { "l2rCompass", c.l2rCompass }, { "r2lCompass", c.r2lCompass }, { "legal", c.legal },
			{ "framesL2R", c.framesL2R }, { "framesR2L", c.framesR2L }, { "calSpeed", c.calSpeed }, { "calFps", c.calFps },
			{ "minLoggedSpeed", c.minLoggedSpeed } } },
		{ "moved", in.moved }, { "files", in.files }, { "vehicles", vs },
	};
}

// Fills `out` from archive.json, keeping defaults for anything missing or of the wrong type (hand-edited files).
static void fromJson(const json& j, Info& out) {
	auto str = [](const json& o, const char* k, const std::string& d) { return o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : d; };
	auto num = [](const json& o, const char* k, double d) { return o.contains(k) && o[k].is_number() ? o[k].get<double>() : d; };
	auto flag = [](const json& o, const char* k) { return o.contains(k) && o[k].is_boolean() && o[k].get<bool>(); };
	out.name = str(j, "name", "");
	out.created = str(j, "created", "");
	out.from = str(j, "from", "");
	out.siteName = str(j, "siteName", "");
	out.speedLimit = int(num(j, "speedLimit", 25));
	out.wayOver = int(num(j, "wayOver", 35));
	out.vst = str(j, "vst", "");
	out.configPath = str(j, "configPath", "");
	out.moved = flag(j, "moved");
	if (j.contains("calibration") && j["calibration"].is_object()) {
		const json& c = j["calibration"];
		Calibration& k = out.cal;
		k.l2rCompass = str(c, "l2rCompass", k.l2rCompass);
		k.r2lCompass = str(c, "r2lCompass", k.r2lCompass);
		k.legal = str(c, "legal", k.legal);
		k.framesL2R = int(num(c, "framesL2R", k.framesL2R));
		k.framesR2L = int(num(c, "framesR2L", k.framesR2L));
		k.calSpeed = num(c, "calSpeed", k.calSpeed);
		k.calFps = num(c, "calFps", k.calFps);
		k.minLoggedSpeed = int(num(c, "minLoggedSpeed", k.minLoggedSpeed));
	}
	out.files.clear();
	if (j.contains("files") && j["files"].is_array())
		for (const json& f : j["files"]) if (f.is_string()) out.files.push_back(f.get<std::string>());
	out.vehicles.clear();
	if (j.contains("vehicles") && j["vehicles"].is_array())
		for (const json& o : j["vehicles"]) {
			if (!o.is_object()) continue;
			Vehicle v;
			v.passed = str(o, "passed", "");
			v.speed = int(num(o, "speed", 0));
			v.l2r = str(o, "direction", "L2R") != "R2L";
			v.compass = str(o, "compass", "");
			v.wrongWay = flag(o, "wrongWay");
			v.frames = int(num(o, "frames", 0));
			v.area = int(num(o, "area", 0));
			v.frame = int(num(o, "frame", 0));
			v.large = flag(o, "large");
			v.flagged = flag(o, "flagged");
			v.fps = num(o, "fps", 30);
			v.source = str(o, "source", "");
			v.snapshot = str(o, "snapshot", "");
			v.clip = str(o, "clip", "");
			v.lengthFt = num(o, "lengthFt", 0);
			v.trend = str(o, "trend", "");
			v.entryMph = int(num(o, "entryMph", 0));
			v.exitMph = int(num(o, "exitMph", 0));
			v.accel = num(o, "accel", 0);
			v.mphIn20ft = int(num(o, "mphIn20ft", 0));
			v.ownDrive = int(num(o, "ownDrive", 0));
			v.drivenMph = num(o, "drivenMph", 0);
			out.vehicles.push_back(v);
		}
	out.firstPassed.clear();
	out.lastPassed.clear();
	for (const Vehicle& v : out.vehicles) {
		if (v.passed.empty()) continue;
		if (out.firstPassed.empty() || v.passed < out.firstPassed) out.firstPassed = v.passed;
		if (out.lastPassed.empty() || v.passed > out.lastPassed) out.lastPassed = v.passed;
	}
}

static std::string vehiclesCsv(const Info& in) {
	std::ostringstream o;
	// New columns go at the end, so a spreadsheet made from an older vehicles.csv keeps its column letters.
	o << "#,Passed,Speed (mph),Direction,Compass,Wrong way,Frames,Size,Truck or bus,Flagged,Source,Snapshot,Clip,"
		"Length (ft),In (mph),Out (mph),Accel (mph/s),Trend,In 20 ft (mph),Own drive\n";
	for (size_t i = 0; i < in.vehicles.size(); i++) {
		const Vehicle& v = in.vehicles[i];
		o << i + 1 << ',' << cell(v.passed) << ',' << v.speed << ',' << (v.l2r ? "L2R" : "R2L") << ',' << cell(v.compass) << ','
			<< (v.wrongWay ? "yes" : "") << ',' << v.frames << ',' << v.area << ',' << (v.large ? "yes" : "") << ','
			<< (v.flagged ? "yes" : "") << ',' << cell(v.source) << ',' << cell(v.snapshot) << ',' << cell(v.clip) << ',';
		if (v.lengthFt > 0) o << std::round(v.lengthFt * 10) / 10;
		o << ',';
		if (!v.trend.empty()) o << v.entryMph << ',' << v.exitMph << ',' << v.accel << ',' << v.trend << ',' << v.mphIn20ft;
		else o << ",,,,";
		o << ',' << (v.ownDrive == 1 ? "calibration" : v.ownDrive == 2 ? "check" : "") << '\n';
	}
	return o.str();
}

// ---- write -------------------------------------------------------------------------------------------------

bool write(const Request& req, std::string& dir, std::string& error, std::vector<std::string>* warnings) {
	auto warn = [&](const std::string& w) { if (warnings) warnings->push_back(w); };
	std::error_code ec;
	fs::path root = fs::path(req.dataDir) / req.root;
	fs::create_directories(root, ec);
	if (ec) { error = "Couldn't make the " + req.root + " folder in " + req.dataDir + ": " + ec.message(); return false; }
	fs::path final_(folderFor(root.string(), req.info.created, req.info.name));
	fs::path tmp = final_.string() + ".partial";
	fs::remove_all(tmp, ec);
	for (const char* sub : { "Stats", "HiLites", "Clips", "Snapshots" }) fs::create_directories(tmp / sub, ec);
	if (ec) { error = "Couldn't make " + tmp.string() + ": " + ec.message(); return false; }
	auto fail = [&](const std::string& why) { error = why; std::error_code e2; fs::remove_all(tmp, e2); return false; };

	// The originals a move takes out of the data folder once the archive is safely in place.
	std::vector<fs::path> originals;

	// Stats csvs, each with its manifest and highlights video when there is one.  The same csv named twice is copied once.
	std::set<std::string> seen;
	for (const std::string& csv : req.statsCsvs) {
		fs::path p(csv);
		if (!seen.insert(p.filename().string()).second) continue;
		if (!fs::is_regular_file(p, ec)) { warn("The results file " + p.filename().string() + " is gone; its vehicles are archived without it."); continue; }
		if (!copyInto(p, tmp / "Stats" / p.filename(), error)) return fail(error);
		originals.push_back(p);
		fs::path m(manifestFor(csv));
		if (fs::is_regular_file(m, ec)) {
			if (!copyInto(m, tmp / "Stats" / m.filename(), error)) return fail(error);
			originals.push_back(m);
		}
		fs::path h(highlightsFor(csv));
		if (fs::is_regular_file(h, ec)) {
			if (!copyInto(h, tmp / "HiLites" / h.filename(), error)) return fail(error);
			originals.push_back(h);
		}
	}
	if (!req.configFile.empty() && fs::is_regular_file(req.configFile, ec)) {
		if (!copyInto(req.configFile, tmp / "VST.cfg", error)) return fail(error);
	}
	else warn("The config file wasn't found, so the archive has no VST.cfg.");
	for (const std::string& h : req.historyFiles)
		if (fs::is_regular_file(h, ec)) {
			if (!copyInto(h, tmp / "calibration-history.csv", error)) return fail(error);
			break;
		}

	// Vehicles: the snapshot as a JPEG named by row number, the clip under its own name.  imencode + ofstream rather than
	// imwrite, which can't open non-ASCII paths on Windows.
	Info info = req.info;
	info.vehicles.clear();
	for (size_t i = 0; i < req.vehicles.size(); i++) {
		const Source& s = req.vehicles[i];
		Vehicle v = s.row;
		v.snapshot.clear();
		v.clip.clear();
		if (!s.snapshot.empty()) {
			std::vector<uchar> jpg;
			std::string rel = "Snapshots/vehicle_" + std::to_string(i + 1) + ".jpg";
			if (cv::imencode(".jpg", s.snapshot, jpg, { cv::IMWRITE_JPEG_QUALITY, 90 })
				&& writeText(tmp / rel, std::string(jpg.begin(), jpg.end())))
				v.snapshot = rel;
			else warn("Vehicle " + std::to_string(i + 1) + "'s snapshot couldn't be saved.");
		}
		if (!s.clipPath.empty()) {
			fs::path c(s.clipPath);
			if (fs::is_regular_file(c, ec)) {
				std::string rel = "Clips/" + c.filename().string();
				if (!copyInto(c, tmp / rel, error)) return fail(error);
				v.clip = rel;
				originals.push_back(c);
			}
		}
		info.vehicles.push_back(v);
	}
	info.moved = req.move;
	info.dir = final_.string();
	fromJson(toJson(info), info);   // works out firstPassed/lastPassed the same way read() does

	if (!writeText(tmp / "vehicles.csv", vehiclesCsv(info))) return fail("Couldn't write vehicles.csv in " + tmp.string());
	for (const auto& x : req.extra) {
		fs::path p = tmp / fs::path(x.first);
		fs::create_directories(p.parent_path(), ec);
		if (!writeText(p, x.second)) return fail("Couldn't write " + x.first + " in " + tmp.string());
	}
	if (!writeText(tmp / "review.html", reviewHtml(info))) return fail("Couldn't write review.html in " + tmp.string());
	info.files.clear();
	for (auto it = fs::recursive_directory_iterator(tmp, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		if (it->is_regular_file(ec)) info.files.push_back(fs::relative(it->path(), tmp, ec).generic_string());
	if (req.checksums) info.files.push_back("SHA256SUMS");
	std::sort(info.files.begin(), info.files.end());
	if (!writeText(tmp / "archive.json", toJson(info).dump(1, '\t') + "\n")) return fail("Couldn't write archive.json in " + tmp.string());
	if (req.checksums) {
		// Every file but review.html (a view, rewritten when a check is added) and SHA256SUMS itself; archive.json included.
		std::vector<std::string> summed;
		for (const std::string& f : info.files) if (f != "review.html" && f != "SHA256SUMS") summed.push_back(f);
		summed.push_back("archive.json");
		std::sort(summed.begin(), summed.end());
		std::string sums = checksumsFor(tmp.string(), summed);
		if (sums.empty() || !writeText(tmp / "SHA256SUMS", sums)) return fail("Couldn't write SHA256SUMS in " + tmp.string());
	}

	fs::rename(tmp, final_, ec);
	if (ec) return fail("Couldn't finish the archive at " + final_.string() + ": " + ec.message());
	dir = final_.string();

	// Only now, with every file safely in the archive, take the originals away.  A file that can't be removed (in use)
	// stays where it was: a warning, not a failure, since the archive is complete.
	if (req.move)
		for (const fs::path& p : originals) {
			std::error_code e2;
			fs::remove(p, e2);
			if (e2) warn("Couldn't take " + p.filename().string() + " out of your data folder (" + e2.message() + "); the archive has a copy.");
		}
	return true;
}

std::string checksumsFor(const std::string& dir, const std::vector<std::string>& files) {
	std::string out;
	for (const std::string& f : files) {
		std::string h = sha256::hexOfFile((fs::path(dir) / fs::path(f)).string());
		if (h.empty()) return "";
		out += h + "  " + f + "\n";
	}
	return out;
}

// ---- read, list, restore -----------------------------------------------------------------------------------

bool read(const std::string& path, Info& out, std::string& error) {
	fs::path dir(path);
	if (dir.filename() == "archive.json") dir = dir.parent_path();
	std::ifstream in(dir / "archive.json");
	if (!in) { error = "No archive.json in " + dir.string() + ". Is this an archive folder?"; return false; }
	json j = json::parse(in, nullptr, false);
	if (j.is_discarded() || !j.is_object() || !j.contains("format") || j["format"] != "vst-archive") {
		error = (dir / "archive.json").string() + " isn't a VST archive.";
		return false;
	}
	out = Info();
	fromJson(j, out);
	out.dir = dir.string();
	return true;
}

std::vector<Info> list(const std::string& dataDir) {
	std::vector<Info> out;
	std::error_code ec;
	for (auto it = fs::directory_iterator(fs::path(dataDir) / "Archive", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
		if (!it->is_directory(ec) || it->path().extension() == ".partial") continue;
		Info a;
		std::string err;
		if (read(it->path().string(), a, err)) out.push_back(a);
	}
	std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) { return a.created != b.created ? a.created > b.created : a.dir > b.dir; });
	return out;
}

bool restore(const std::string& dir, const std::string& dataDir, std::string& error) {
	Info a;
	if (!read(dir, a, error)) return false;
	std::error_code ec;
	std::vector<std::string> kept;
	for (const std::string& rel : a.files) {
		if (rel.rfind("Stats/", 0) != 0 && rel.rfind("HiLites/", 0) != 0) continue;
		if (rel == "Stats/calibration-sets.json") continue;   // the data folder keeps its own; App merges the archived sets back
		fs::path dst = fs::path(dataDir) / fs::path(rel);
		fs::create_directories(dst.parent_path(), ec);
		if (fs::exists(dst, ec)) { kept.push_back(dst.filename().string()); continue; }
		if (!copyInto(fs::path(a.dir) / fs::path(rel), dst, error)) return false;
	}
	// The archive stays complete (snapshots, clips and its own copy of every file); it just no longer stands in for them.
	a.moved = false;
	std::ifstream in(fs::path(a.dir) / "archive.json");
	json j = json::parse(in, nullptr, false);
	in.close();
	if (j.is_object()) {
		j["moved"] = false;
		writeText(fs::path(a.dir) / "archive.json", j.dump(1, '\t') + "\n");
	}
	error.clear();
	if (!kept.empty()) {
		error = "Already in your data folder, so left as it is:";
		for (const std::string& k : kept) error += " " + k;
	}
	return true;
}

// ---- review.html -------------------------------------------------------------------------------------------

std::string reviewHtml(const Info& info) {
	// The data goes in as JSON inside a script tag; "</" is escaped so a source name can't close the tag.
	std::string data = toJson(info).dump();
	for (size_t p = 0; (p = data.find("</", p)) != std::string::npos; p += 3) data.replace(p, 2, "<\\/");
	std::string html = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>VST archive</title>
<style>
:root{--bg:#f6f7f9;--card:#fff;--text:#1b1f24;--dim:#5b6470;--line:#e2e5ea;--ok:#1f9d55;--warn:#c27c0e;--bad:#d93a3a;--wrong:#8a3ffc;--acc:#2563eb}
@media (prefers-color-scheme:dark){:root{--bg:#121417;--card:#1b1e23;--text:#e8eaed;--dim:#9aa3ad;--line:#2c3138;--acc:#6ea0ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
a{color:var(--acc)}main{max-width:1200px;margin:0 auto;padding:20px 16px}h1{font-size:22px;margin:0 0 2px}.dim{color:var(--dim)}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px;margin:16px 0}
.tile{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:10px 12px}.tile b{display:block;font-size:24px}
.bar{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin:8px 0}
button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--text);border-radius:8px;padding:5px 12px;cursor:pointer}
button.on{background:var(--acc);border-color:var(--acc);color:#fff}
.wrap{display:grid;grid-template-columns:1fr 340px;gap:16px}@media (max-width:820px){.wrap{grid-template-columns:1fr}}
table{width:100%;border-collapse:collapse;background:var(--card);border:1px solid var(--line);border-radius:10px;overflow:hidden}
th,td{padding:7px 10px;text-align:left;border-bottom:1px solid var(--line)}th{cursor:pointer;font-size:13px;color:var(--dim)}
tr.sel td{background:color-mix(in srgb,var(--acc) 14%,transparent)}tbody tr{cursor:pointer}
.pill{display:inline-block;white-space:nowrap;border-radius:999px;padding:1px 9px;font-weight:600;color:#fff;font-size:13px}
@media (max-width:560px){.src{display:none}th,td{padding:7px 6px}}
.det{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;position:sticky;top:12px;align-self:start}
.det img,.det video{width:100%;border-radius:8px;background:#000}.big{font-size:44px;font-weight:700}
#cover{display:none;margin:14px 0 4px}.badge{display:inline-flex;gap:8px;align-items:center;border-radius:999px;padding:4px 14px;font-weight:700;color:#fff;font-size:16px}
.lead{font-size:17px;margin:8px 0 2px;max-width:60em}details{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:8px 12px;margin:8px 0}
summary{cursor:pointer;font-weight:600}details table{margin:8px 0;display:block;overflow-x:auto}code{font:13px ui-monospace,Menlo,Consolas,monospace}
</style></head><body><main>
<h1 id="title"></h1><div class="dim" id="sub"></div>
<section id="cover"></section>
<div class="tiles" id="tiles"></div>
<div class="bar" id="filters"></div>
<div class="wrap"><div><table><thead><tr><th data-k="n">#</th><th data-k="speed">Speed</th><th>Direction</th><th data-k="passed">Passed</th><th class="src">Source</th></tr></thead><tbody id="rows"></tbody></table></div>
<div class="det" id="det"><span class="dim">Pick a vehicle to see its snapshot and clip.</span></div></div>
<p class="dim">Archived by VST. Snapshots here are not blurred: share this folder only with people you'd show the street to.</p>
</main>
<script id="data" type="application/json">@DATA@</script>
<script id="dataset" type="application/json">@DATASET@</script>
<script>
const A=JSON.parse(document.getElementById('data').textContent),L=A.speedLimit,W=A.wayOver;
const V=A.vehicles.map((v,i)=>Object.assign({n:i+1},v));
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const col=v=>v.wrongWay?'var(--wrong)':v.speed>=W?'var(--bad)':v.speed>L?'var(--warn)':'var(--ok)';
const when=v=>v.passed?v.passed.replace('T',' '):(v.fps>0?Math.floor(v.frame/v.fps/60)+':'+String(Math.floor(v.frame/v.fps)%60).padStart(2,'0')+' in video':'');
document.getElementById('title').textContent=A.name||'Archived dataset';
document.title=(A.name||'Archive')+' · VST';
document.getElementById('sub').textContent=[A.siteName,(document.getElementById('dataset').textContent.trim()!=='null'?'published ':'archived ')+(A.created||'').slice(0,10),A.from?'from '+(A.from=='This session'?'this session':A.from):'','limit '+L+' mph'].filter(Boolean).join(' · ');
const T=V.filter(v=>!v.ownDrive);  // traffic: the tiles leave out the user's own calibration and check drives
const sp=T.map(v=>v.speed).sort((a,b)=>a-b),p85=sp.length?sp[Math.ceil(.85*sp.length)-1]:0;
const tile=(k,v,s)=>`<div class="tile"><span class="dim">${k}</span><b>${v}</b><span class="dim">${s}</span></div>`;
document.getElementById('tiles').innerHTML=tile('Vehicles',T.length,V.length>T.length?(V.length-T.length)+' own drives left out':'')+tile('Speeding',T.filter(v=>v.speed>L).length,'over '+L+' mph')
 +tile('Way over',T.filter(v=>v.speed>=W).length,W+' mph or more')+tile('Wrong way',T.filter(v=>v.wrongWay).length,'')+tile('85th percentile',p85+' mph','');
const F=[['All',v=>true],['Speeding',v=>v.speed>L],['Way over',v=>v.speed>=W],['Wrong way',v=>v.wrongWay]];
let f=0,key='speed',asc=false,sel=0;
function draw(){
 document.getElementById('filters').innerHTML=F.map((x,i)=>`<button class="${i==f?'on':''}" onclick="f=${i};draw()">${x[0]}</button>`).join('');
 const r=V.filter(F[f][1]).sort((a,b)=>{const x=a[key],y=b[key];return (x<y?-1:x>y?1:0)*(asc?1:-1)});
 document.getElementById('rows').innerHTML=r.map(v=>`<tr class="${v.n==sel?'sel':''}" onclick="pick(${v.n})"><td>${v.n}</td>`
  +`<td><span class="pill" style="background:${col(v)}">${v.speed} mph</span></td><td>${v.direction=='R2L'?'←':'→'} ${esc(v.compass)}${v.wrongWay?' <span class="pill" style="background:var(--wrong)">wrong way</span>':''}${v.ownDrive?' <span class="pill" style="background:var(--acc)">'+(v.ownDrive==2?'check':'calibration')+'</span>':''}</td>`
  +`<td>${esc(when(v))}</td><td class="dim src">${esc(v.source)}${v.flagged?' ⚠':''}</td></tr>`).join('');
}
function pick(n){sel=n;const v=V[n-1];draw();
 document.getElementById('det').innerHTML=(v.snapshot?`<img src="${esc(v.snapshot)}" alt="snapshot">`:'<p class="dim">No snapshot for this vehicle.</p>')
 +`<div class="big" style="color:${col(v)}">${v.speed} <span class="dim" style="font-size:16px">mph</span></div>`
 +`<div>${v.direction=='R2L'?'Right to left':'Left to right'}${v.compass?', '+esc(v.compass):''}${v.wrongWay?' · <b>wrong way</b>':''}</div>`
 +`<div class="dim">${esc(when(v))} · ${v.frames} frames · ${v.lengthFt>0?Math.round(v.lengthFt)+' ft long':'size '+v.area}${v.large?' (truck or bus)':''}</div>`
 +(v.trend?`<div class="dim">${v.entryMph} → ${v.exitMph} mph in the zone (${v.accel>0?'+':''}${v.accel} mph/s, ${esc(v.trend)}), ${v.mphIn20ft} mph 20 ft on</div>`:'')
 +(v.ownDrive?`<div><b>Your own ${v.ownDrive==2?'check':'calibration'} drive</b> at ${v.drivenMph} mph: left out of the counts.</div>`:'')
 +(v.clip?`<p><video src="${esc(v.clip)}" controls muted playsinline></video><br><a href="${esc(v.clip)}">Open the clip</a></p>`:'');
 if(innerWidth<820)document.getElementById('det').scrollIntoView({behavior:'smooth'});}
document.querySelectorAll('th[data-k]').forEach(th=>th.onclick=()=>{const k=th.dataset.k;asc=key==k?!asc:k!='speed';key=k;draw()});
draw();
// A published dataset (Dataset.h): three layers.  1, for everyone: the badge and one sentence.  2, how we know: the
// calibration and every re-check, before and after.  3, the numbers: the test, the files, the schema and checksums.
const D=JSON.parse(document.getElementById('dataset').textContent||'null');
if(D){
 const look={accurate:['var(--ok)','\u2714'],review:['var(--warn)','?'],tainted:['var(--bad)','!'],'':['var(--dim)','\u2022']}[D.status||''];
 const pct=x=>(x*100).toFixed(1)+'%',sg=x=>(x>0?'+':'')+(x*100).toFixed(1)+'%',dirName=d=>d=='L2R'?'Left to right ('+esc(D.directions.L2R)+')':'Right to left ('+esc(D.directions.R2L)+')';
 const sig=x=>x>0?pct(x):'not measured (6% assumed)';
 const C=D.checks||[],last=C[C.length-1];
 let h=`<span class="badge" style="background:${look[0]}">${look[1]} ${esc(D.statusLabel)}</span><p class="lead">${esc(D.sentence)}</p>`
  +`<div class="dim">Published ${esc((D.published||'').slice(0,10))} · version ${D.version}${D.correctionOf?' · corrects '+esc(D.correctionOf):''} · speeds accurate ${esc(D.accuracy)}</div>`;
 h+=`<details><summary>How we know</summary><p>The camera times each car between two lines a known distance apart on the street. That distance was set by calibration (${esc(D.calibration.how||'one calibration, method not recorded')}). `
  +`Speeds are accurate ${esc(D.accuracy)}, with 95% confidence.${D.calibration.sigma.L2R>0&&D.calibration.sigma.R2L>0?'':' How closely that calibration was measured isn\u2019t on record, so a cautious \u00b16% (one standard deviation) is assumed; that is why the band is wide.'}</p>`;
 if(!C.length)h+='<p>No newer calibration has been compared with it yet.</p>';
 else{h+='<table><thead><tr><th>Re-checked</th><th>Result</th><th>Cars over the limit</th><th>85th percentile</th><th>Average change</th></tr></thead><tbody>'
  +C.map(c=>`<tr><td>${esc(c.when.slice(0,10))}</td><td>${esc({accurate:'Still accurate',review:'Under review',tainted:'Tainted: needs correction'}[c.verdict]||'')}</td><td>${c.before.over} \u2192 ${c.after.over}</td><td>${c.before.p85} \u2192 ${c.after.p85} mph</td><td>${c.meanDiff>0?'+':''}${c.meanDiff.toFixed(1)} mph</td></tr>`).join('')
  +'</tbody></table><p class="dim">A newer calibration changes every speed by the same percentage, because it only changes the measured distance. Each re-check lists every car before and after.</p>';}
 h+='</details>';
 h+=`<details><summary>The numbers</summary><p>Each speed is the zone length divided by the time the car took (<code>zoneSeconds</code> in points.csv): speed = zone ft \u00f7 seconds \u00f7 1.4667. `
  +`Zone: ${D.calibration.zoneFt.L2R} ft left to right, ${D.calibration.zoneFt.R2L} ft right to left; relative 1\u03c3 ${sig(D.calibration.sigma.L2R)} and ${sig(D.calibration.sigma.R2L)}; promised 95% band T = 1.96\u03c3.</p>`
  +'<p>A re-check compares a newer calibration of the same view: \u03b4 = new zone \u00f7 published zone \u2212 1. Two one-sided tests at 95% (TOST) with the new calibration\u2019s \u03c3\u2099: still accurate when |\u03b4| + 1.645\u03c3\u2099 \u2264 T, tainted when |\u03b4| \u2212 1.645\u03c3\u2099 &gt; T, otherwise under review. The worse direction decides.</p>';
 if(last)h+='<table><thead><tr><th>Direction</th><th>\u03b4</th><th>90% interval</th><th>T</th><th>\u03c3\u2099</th><th>Verdict</th></tr></thead><tbody>'
  +['L2R','R2L'].filter(d=>last.test[d].verdict).map(d=>{const t=last.test[d];return `<tr><td>${dirName(d)}</td><td>${sg(t.delta)}</td><td>${sg(t.lo)} to ${sg(t.hi)}</td><td>\u00b1${pct(t.tolerance)}</td><td>${pct(t.sigmaNew)}</td><td>${esc(t.verdict)}</td></tr>`}).join('')+'</tbody></table>';
 h+=`<p>Files: <a href="points.csv">points.csv</a> (one row per car), <a href="dataset.json">dataset.json</a>, ${C.map(c=>`<a href="${esc(c.file)}">${esc(c.file)}</a>`).join(', ')}${C.length?', ':''}<a href="SHA256SUMS">SHA256SUMS</a>. `
  +`Format <code>${esc(D.schema)}</code>, documented in docs/dataset-schema.md in VST\u2019s source. Check that nothing was changed with <code>sha256sum -c SHA256SUMS</code> in this folder.</p></details>`;
 const cv=document.getElementById('cover');cv.innerHTML=h;cv.style.display='block';
 document.title=(D.name||'Dataset')+' · VST';
}
</script></body></html>
)HTML";
	std::string ds = info.dataset.empty() ? std::string("null") : info.dataset;
	for (size_t p = 0; (p = ds.find("</", p)) != std::string::npos; p += 3) ds.replace(p, 2, "<\\/");
	html.replace(html.find("@DATASET@"), 9, ds);
	html.replace(html.find("@DATA@"), 6, data);
	return html;
}

}  // namespace archive
