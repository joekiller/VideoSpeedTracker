#include "CalEvidence.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace calib {

Combined combine(const std::vector<Input>& inputs, bool l2r, const std::string& lines) {
	Combined c;
	// Gather what counts: this direction, not removed, a usable value, and measured on these speed lines (an input
	// with no lines on record, such as one seeded from an old calibration, is taken as made on the current ones).
	std::vector<const Input*> use;
	for (const Input& in : inputs) {
		if (in.removed || !in.appliesTo(l2r) || !(in.zoneFt > 0) || !(in.sigma > 0)) continue;
		if (!in.lines.empty() && !lines.empty() && in.lines != lines) { c.setAside++; continue; }
		use.push_back(&in);
	}
	c.n = int(use.size());
	if (!c.n) return c;

	// Inverse-variance weighted mean.  The sigmas are relative and the values within a few percent of each other, so
	// the weights are w = 1/σ² (equal sigmas give the plain mean) and the combination's relative sigma is 1/sqrt(Σw).
	double sw = 0, swx = 0;
	std::vector<double> w(use.size());
	for (size_t i = 0; i < use.size(); i++) {
		w[i] = 1 / (use[i]->sigma * use[i]->sigma);
		sw += w[i];
		swx += w[i] * use[i]->zoneFt;
	}
	c.valid = true;
	c.zoneFt = swx / sw;
	double chi2 = 0;
	for (size_t i = 0; i < use.size(); i++) {
		double r = use[i]->zoneFt / c.zoneFt - 1;
		chi2 += w[i] * r * r;
	}
	c.birge = c.n > 1 ? std::sqrt(chi2 / (c.n - 1)) : 1;
	c.sigma = 1 / std::sqrt(sw) * std::max(1.0, c.birge);

	// The spread a novice reads as "agree within X%": the range of the inputs, as a share of the result.
	double lo = use[0]->zoneFt, hi = lo;
	for (const Input* in : use) { lo = std::min(lo, in->zoneFt); hi = std::max(hi, in->zoneFt); }
	c.spreadPct = (hi - lo) / c.zoneFt * 100;

	for (size_t i = 0; i < use.size(); i++) {
		Share s;
		s.id = use[i]->id;
		s.contribution = w[i] / sw;
		s.offPct = (use[i]->zoneFt / c.zoneFt - 1) * 100;
		// Leave-one-out: with three or more, an input can be compared with what the others say without itself.  It is
		// an outlier when it is off them by more than 3 sigma (its own and theirs together) and by kOutlierPct, and the
		// others agree among themselves (spread within 3% or Birge ratio up to 1.5); when they all disagree there is
		// no single odd one out, and the summary says they differ instead.  With two, either could be the wrong one.
		if (c.n >= 3) {
			double ow = sw - w[i], ox = (swx - w[i] * use[i]->zoneFt) / ow;
			double sigOthers = 1 / std::sqrt(ow), dev = std::fabs(use[i]->zoneFt / ox - 1);
			double ochi2 = 0, ospread = 0;
			for (size_t j = 0; j < use.size(); j++) {
				if (j == i) continue;
				double r = use[j]->zoneFt / ox - 1;
				ochi2 += w[j] * r * r;
				ospread = std::max(ospread, std::fabs(r));
			}
			bool othersAgree = ospread * 100 <= 3.0 + 1e-9 || std::sqrt(ochi2 / (c.n - 2)) <= 1.5;
			s.outlier = othersAgree && dev > 3 * std::hypot(use[i]->sigma, sigOthers) && dev * 100 > kOutlierPct;
			c.outliers += s.outlier;
		}
		c.shares.push_back(s);
	}
	return c;
}

std::string summary(const Combined& c, std::string* detail) {
	auto say = [detail](const char* d) { if (detail) *detail = d; };
	char b[160];
	if (!c.valid) {
		if (c.setAside) {
			say("They were made with the speed lines somewhere else. Measure again on these lines; one drive past is enough.");
			return "Your earlier measurements don't fit these speed lines";
		}
		say("");
		return "Not calibrated yet";
	}
	int within = std::max(1, int(std::ceil(c.spreadPct - 1e-9)));
	if (c.n == 1) {
		say("Add another way (a drive past, cones, or a car you know) and VST cross-checks them.");
		return "1 measurement so far";
	}
	if (c.outliers) {
		if (c.outliers == 1) snprintf(b, sizeof b, "%d measurements, 1 well off the others", c.n);
		else snprintf(b, sizeof b, "%d measurements, %d well off the rest", c.n, c.outliers);
		say(c.outliers == 1 ? "Take out the one marked if you doubt it, or add another measurement to settle it."
			: "Take out the ones marked if you doubt them, or add another measurement to settle it.");
		return b;
	}
	if (c.agree()) {
		snprintf(b, sizeof b, "%d measurements agree within %d%%", c.n, within);
		say("");
		return b;
	}
	snprintf(b, sizeof b, "%d measurements differ by %d%%", c.n, within);
	say(c.n == 2 ? "VST uses a weighted middle. A third measurement will show which one is off."
		: "VST uses a weighted middle. Remove one you doubt, or add another.");
	return b;
}

const char* methodName(const std::string& m) {
	if (m == "pass") return "Drive past";
	if (m == "check") return "Check drive";
	if (m == "cones") return "Cones or pole";
	if (m == "car") return "Known car";
	if (m == "camera") return "Camera setup";
	if (m == "earlier") return "Earlier calibration";
	return "Measurement";
}

static std::string canonical(const std::string& p) {
	if (p.empty()) return p;
	std::error_code ec;
	fs::path c = fs::weakly_canonical(fs::path(p), ec);
	return ec ? p : c.string();
}

static const char* dirsName(int d) { return d == kL2R ? "L2R" : d == kR2L ? "R2L" : "both"; }
static int dirsFrom(const std::string& s) { return s == "L2R" ? kL2R : s == "R2L" ? kR2L : kBothDirs; }

static json readFile(const std::string& path) {
	std::ifstream f(path, std::ios::binary);
	if (!f) return json::object();
	json j = json::parse(f, nullptr, false);   // no exceptions: a damaged file reads as empty, never crashes Studio
	return j.is_object() ? j : json::object();
}

std::vector<Input> loadInputs(const std::string& path, const std::string& configPath, bool* known) {
	std::vector<Input> out;
	if (known) *known = false;
	json j = readFile(path);
	if (!j.contains("configs") || !j["configs"].is_object()) return out;
	std::string want = canonical(configPath);
	for (auto& [key, list] : j["configs"].items()) {
		if (canonical(key) != want || !list.is_array()) continue;
		if (known) *known = true;
		for (const json& e : list) {
			if (!e.is_object()) continue;
			Input in;
			in.id = e.value("id", "");
			in.method = e.value("method", "");
			in.label = e.value("label", "");
			in.time = e.value("time", "");
			in.dirs = dirsFrom(e.value("dirs", "both"));
			in.zoneFt = e.contains("zoneFt") && e["zoneFt"].is_number() ? e["zoneFt"].get<double>() : 0;
			in.sigma = e.contains("sigma") && e["sigma"].is_number() ? e["sigma"].get<double>() : 0;
			in.lines = e.value("lines", "");
			in.removed = e.value("removed", false);
			if (in.id.empty() || !(in.zoneFt > 0) || !(in.sigma > 0)) continue;   // unusable: skip, keep the rest
			out.push_back(in);
		}
	}
	return out;
}

bool saveInputs(const std::string& path, const std::string& configPath, const std::vector<Input>& inputs, std::string& err) {
	json j = readFile(path);
	if (!j.contains("configs") || !j["configs"].is_object()) j["configs"] = json::object();
	j["version"] = 1;
	// Drop any spelling of this config's path before writing it under its canonical one.
	std::string want = canonical(configPath);
	std::vector<std::string> stale;
	for (auto& [key, list] : j["configs"].items()) if (canonical(key) == want) stale.push_back(key);
	for (const std::string& k : stale) j["configs"].erase(k);
	json list = json::array();
	for (const Input& in : inputs) {
		json e{ { "id", in.id }, { "method", in.method }, { "label", in.label }, { "time", in.time }, { "dirs", dirsName(in.dirs) },
			{ "zoneFt", std::round(in.zoneFt * 1000) / 1000 }, { "sigma", std::round(in.sigma * 10000) / 10000 }, { "lines", in.lines } };
		if (in.removed) e["removed"] = true;
		list.push_back(e);
	}
	j["configs"][want] = list;

	std::error_code ec;
	fs::create_directories(fs::path(path).parent_path(), ec);
	std::string tmp = path + ".tmp";
	{
		std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
		if (!f) { err = "Can't write " + tmp; return false; }
		f << j.dump(1, '\t') << "\n";
		f.flush();
		if (!f) { err = "Can't write " + tmp; return false; }
	}
	fs::rename(tmp, path, ec);   // replaces atomically on POSIX; on Windows rename over an existing file also works here
	if (ec) {
		// Some Windows filesystems refuse to rename over a file: fall back to remove-then-rename.
		std::error_code ec2;
		fs::remove(path, ec2);
		fs::rename(tmp, path, ec2);
		if (ec2) { err = "Can't write " + path + ": " + ec2.message(); return false; }
	}
	return true;
}

bool addInput(std::vector<Input>& inputs, Input in) {
	for (const Input& e : inputs)
		if (!e.removed && e.method == in.method && e.dirs == in.dirs && e.lines == in.lines
			&& std::fabs(e.zoneFt - in.zoneFt) < 1e-4 * in.zoneFt && std::fabs(e.sigma - in.sigma) < 1e-4) return false;
	in.id = newInputId(inputs, in.time);
	inputs.push_back(in);
	return true;
}

std::string newInputId(const std::vector<Input>& inputs, const std::string& time) {
	for (int k = 1;; k++) {
		std::string id = time + "#" + std::to_string(k);
		if (std::none_of(inputs.begin(), inputs.end(), [&](const Input& in) { return in.id == id; })) return id;
	}
}

}  // namespace calib
