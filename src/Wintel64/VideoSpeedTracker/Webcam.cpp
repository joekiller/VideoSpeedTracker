// Choosing a camera's mode, its settings in the source name, and the registry (Webcam.h).  The platform listers are in
// WebcamList.cpp (Linux, Windows) and WebcamMac.mm (macOS).
#include "Webcam.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace webcam {

// ---------------------------------------------------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------------------------------------------------

bool Mode::usable() const {
	// What OpenCV's V4L2, MSMF and AVFoundation backends decode or convert.  Formats VST has never seen (a camera's own
	// fourcc) are tried too rather than dropped: only those known to fail are left out.
	static const char* const bad[] = { "H264", "H265", "HEVC", "VP80", "VP90", "MPG2",
		"BA81", "GBRG", "GRBG", "RGGB", "BG10", "GB10", "BA10", "RG10", "pBAA", "pGAA", "pgAA", "pRAA", "Y10 ", "Y12 ", "Y16 ", "Z16 " };
	if (!valid() || fps <= 0) return false;
	for (const char* b : bad) if (format == b) return false;
	return true;
}

bool Mode::wide() const { return valid() && std::fabs(double(width) / height - 16.0 / 9.0) < 16.0 / 9.0 * .02; }

std::string Mode::text() const {
	char b[96];
	snprintf(b, sizeof b, "%d × %d at %.0f fps", width, height, fps);
	std::string t = b;
	if (!format.empty()) {
		std::string f = format;
		while (!f.empty() && f.back() == ' ') f.pop_back();
		t += " (" + f + ")";
	}
	return t;
}

namespace {

// best()'s classes (Webcam.h): higher is better.
int klass(const Mode& m) {
	bool hd = m.width >= 1280 && m.height >= 720, sd = m.width >= 640 && m.height >= 480;
	if (m.fps >= 55 && hd) return 5;
	if (m.fps >= 55 && sd) return 4;
	if (m.fps >= 24 && hd) return 3;
	if (m.fps >= 24 && sd) return 2;
	return 1;
}

// 720 best; above it, the nearer the better (1080 before 1440 before 2160); below it, the taller the better.
int heightRank(int h) { return h >= 720 ? h - 720 : 10000 + (720 - h); }

int formatRank(const std::string& f) {
	if (f == "MJPG") return 0;
	if (f == "YUYV" || f == "YUY2") return 1;
	if (f == "NV12" || f == "UYVY" || f == "I420" || f == "YU12") return 2;
	return 3;
}

// True when `a` is the better mode for timing.  Frame rates within a frame of each other count as the same (59.94 is 60).
bool better(const Mode& a, const Mode& b) {
	int ka = klass(a), kb = klass(b);
	if (ka != kb) return ka > kb;
	if (ka == 1) {
		double sa = std::min(a.fps, 120.0) * a.height, sb = std::min(b.fps, 120.0) * b.height;
		if (std::fabs(sa - sb) > 1) return sa > sb;
	}
	if (a.wide() != b.wide()) return a.wide();
	long fa = std::lround(std::min(a.fps, 120.0)), fb = std::lround(std::min(b.fps, 120.0));
	if (std::labs(fa - fb) > 1) return fa > fb;
	int ha = heightRank(a.height), hb = heightRank(b.height);
	if (ha != hb) return ha < hb;
	int ra = formatRank(a.format), rb = formatRank(b.format);
	if (ra != rb) return ra < rb;
	if (a.width != b.width) return a.width > b.width;
	return a.fps > b.fps;
}

}  // namespace

Mode best(const std::vector<Mode>& modes) {
	Mode out;
	for (const Mode& m : modes)
		if (m.usable() && (!out.valid() || better(m, out))) out = m;
	return out;
}

std::string whyBest(const Mode& m, const std::vector<Mode>& modes) {
	if (!m.valid()) return modes.empty() ? "This system didn't list the camera's modes, so VST asks it for 1280 × 720 at 60 fps and uses what it gives."
		: "None of the modes it lists is one VST can read.";
	char b[320];
	switch (klass(m)) {
	case 5: snprintf(b, sizeof b, "The most frames per second it sends at HD size. Speeds are counted in frames, so frames matter most."); break;
	case 4: snprintf(b, sizeof b, "It sends 60 fps only below HD size, and twice the frames times cars better than more pixels."); break;
	case 3: snprintf(b, sizeof b, "It sends at most %.0f fps at HD size: fine for most streets. A 60 fps camera times short views better.", m.fps); break;
	default: snprintf(b, sizeof b, "The best it lists. It works; a camera that sends 1280 × 720 at 30 fps or more does better."); break;
	}
	std::string t = b;
	bool other = false;
	for (const Mode& o : modes) other |= o.usable() && !o.wide() && klass(o) == klass(m);
	if (m.wide() && other) t += " It's 16:9, the shape VST works in, so nothing is stretched.";
	return t;
}

std::vector<Mode> offered(const std::vector<Mode>& modes, size_t limit) {
	std::vector<Mode> out;
	for (const Mode& m : modes) {
		if (!m.usable() || m.width < 320) continue;
		auto same = std::find_if(out.begin(), out.end(), [&](const Mode& o) {
			return o.width == m.width && o.height == m.height && std::lround(o.fps) == std::lround(m.fps);
		});
		if (same == out.end()) out.push_back(m);
		else if (better(m, *same)) *same = m;
	}
	std::stable_sort(out.begin(), out.end(), better);
	if (out.size() > limit) out.resize(limit);
	return out;
}

std::string changeNote(const Mode& was, const Mode& now) {
	if (!was.valid() || !now.valid()) return "";
	if (was.wide() != now.wide())
		return std::string("The picture's shape changes from ") + (was.wide() ? "16:9 to 4:3" : "4:3 to 16:9") + ". Your marks, distance and "
			"calibration are kept, but the box may no longer sit on the lane: look at step 4, Mark your street, after it connects.";
	if (was.width != now.width || was.height != now.height)
		return "Only the size changes. Most cameras show the same view at every size, so your marks and calibration still fit. "
			"A few zoom in at smaller sizes: if the street looks closer after it connects, look at step 4 and drive step 5's check.";
	if (std::lround(was.fps) != std::lround(now.fps))
		return "Only the frame rate changes. Your marks, distance and calibration are kept: speeds are scaled to the frame rate for you.";
	return "";
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings in the source name
// ---------------------------------------------------------------------------------------------------------------------

bool isName(const std::string& name) {
	size_t q = name.find('?');
	std::string head = name.substr(0, q);
	return !head.empty() && head.size() < 4 && std::all_of(head.begin(), head.end(), [](char c) { return std::isdigit((unsigned char)c) != 0; });
}

bool parse(const std::string& name, Settings& s) {
	if (!isName(name)) return false;
	s = Settings();
	size_t q = name.find('?');
	s.camera = std::atoi(name.substr(0, q).c_str());
	if (q == std::string::npos) return true;
	std::string query = name.substr(q + 1);
	for (size_t start = 0; start <= query.size();) {
		size_t amp = query.find('&', start);
		std::string kv = query.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
		size_t eq = kv.find('=');
		std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
		if (k == "w") s.width = std::max(0, std::atoi(v.c_str()));
		else if (k == "h") s.height = std::max(0, std::atoi(v.c_str()));
		else if (k == "fps") s.fps = std::max(0.0, std::atof(v.c_str()));
		else if (k == "fmt") s.format = v.substr(0, 4);
		else if (k == "shutter") s.shutterUs = std::max(-1, std::atoi(v.c_str()));
		if (amp == std::string::npos) break;
		start = amp + 1;
	}
	return true;
}

std::string name(const Settings& s) {
	std::string n = std::to_string(std::max(0, s.camera)), q;
	auto add = [&](const std::string& kv) { q += (q.empty() ? "?" : "&") + kv; };
	if (!s.picksMode()) { add("w=" + std::to_string(s.width)); add("h=" + std::to_string(s.height)); }
	if (s.fps > 0) { char b[32]; snprintf(b, sizeof b, "fps=%g", s.fps); add(b); }
	if (!s.format.empty()) add("fmt=" + s.format);
	if (s.shutterUs >= 0) add("shutter=" + std::to_string(s.shutterUs));
	return n + q;
}

// ---------------------------------------------------------------------------------------------------------------------
// Devices
// ---------------------------------------------------------------------------------------------------------------------

namespace {
std::vector<std::string> words(const std::string& text) {
	std::vector<std::string> out;
	std::string w;
	for (char c : text + " ") {
		if (std::isalnum((unsigned char)c)) w += char(std::tolower((unsigned char)c));
		else if (!w.empty()) { out.push_back(w); w.clear(); }
	}
	return out;
}
}  // namespace

bool Device::isVirtual() const {
	const Model* m = recognize(*this);
	if (m && std::string(m->kind) == "virtual") return true;
	for (const std::string& w : words(name + " " + id)) if (w == "virtual" || w == "v4l2loopback" || w == "dummy") return true;
	return false;
}

std::string Device::label() const { return name.empty() ? "USB camera " + std::to_string(index) : name; }

double Device::topFps() const {
	double hd = 0, any = 0;
	for (const Mode& m : modes) {
		if (!m.usable()) continue;
		any = std::max(any, m.fps);
		if (m.width >= 1280 && m.height >= 720) hd = std::max(hd, m.fps);
	}
	return hd > 0 ? hd : any;
}

namespace {
std::mutex namesMu;
std::map<int, std::string> names;                  // knownName()'s answers, from the last list()
std::map<std::string, std::vector<Mode>> seenModes;  // by Device::id: modes read while the camera was free
}  // namespace

std::recursive_mutex& deviceLock() {
	static std::recursive_mutex m;
	return m;
}

// A camera VST itself is streaming from can't always be asked for its modes again (Windows activates the source to
// read them, and a busy one says no), so the modes it listed while free are kept for it by id.
std::vector<Device> list() {
	std::vector<Device> all;
	{
		std::lock_guard<std::recursive_mutex> busy(deviceLock());
		try { all = listNow(); } catch (...) { all.clear(); }
	}
	// VST_FAKE_WEBCAMS=1: two made-up cameras after the real ones (a C922 and a nameless 4:3 one), so VST Studio's camera
	// menu and settings popup can be seen and screenshotted on a machine with none.  Opening them fails like any absent camera.
	if (const char* fake = std::getenv("VST_FAKE_WEBCAMS"); fake && std::string(fake) == "1") {
		int next = all.empty() ? 0 : all.back().index + 1;
		Device c922;
		c922.index = next; c922.name = "C922 Pro Stream Webcam"; c922.backend = "FAKE"; c922.id = "fake-c922"; c922.vendor = 0x046d; c922.product = 0x085c;
		c922.modes = { { 640, 480, 30, "YUYV" }, { 1280, 720, 10, "YUYV" }, { 640, 480, 30, "MJPG" }, { 1280, 720, 60, "MJPG" }, { 1280, 720, 30, "MJPG" },
			{ 1920, 1080, 30, "MJPG" }, { 800, 600, 30, "MJPG" }, { 960, 720, 30, "MJPG" } };
		Device plain;
		plain.index = next + 1; plain.backend = "FAKE"; plain.id = "fake-plain";
		plain.modes = { { 640, 480, 30, "YUYV" }, { 640, 480, 30, "MJPG" }, { 1024, 768, 15, "MJPG" } };
		all.push_back(c922);
		all.push_back(plain);
	}
	std::lock_guard<std::mutex> l(namesMu);
	names.clear();
	for (Device& d : all) {
		names[d.index] = d.name;
		if (d.id.empty()) continue;
		if (!d.modes.empty()) seenModes[d.id] = d.modes;
		else if (seenModes.count(d.id)) { d.modes = seenModes[d.id]; d.error.clear(); }
	}
	return all;
}

std::string knownName(int index) {
	std::lock_guard<std::mutex> l(namesMu);
	auto it = names.find(index);
	return it == names.end() ? "" : it->second;
}

Device describe(int index) {
	for (Device& d : list()) if (d.index == index) return d;
	Device none;
	none.index = index;
	return none;
}

// ---------------------------------------------------------------------------------------------------------------------
// The registry
// ---------------------------------------------------------------------------------------------------------------------
//
// Field of view is the horizontal angle at the 16:9 modes, from the maker's diagonal figure (78 degrees diagonal is
// about 70 across).  It only seeds step 2's plan: a calibration measures the real view and wins.  Entries are added
// when a camera's report (Connect page, "Report this camera") shows something worth knowing; a camera that isn't
// here works the same, from what its driver lists.

namespace {
const std::vector<Model> kModels = {
	// Suggested: real-deal timing (60 fps or more at HD, or a global shutter).
	{ "Elgato Facecam", "facecam", 0x0fd9, 0x0078, 74, "webcam",
		"1080p at 60 fps with a fixed shutter you can set. Needs a USB 3 port (usually blue) for the 60.", 2, "about $100-150", "Best Buy, Staples" },
	{ "Logitech StreamCam", "streamcam", 0x046d, 0x0893, 70, "webcam", "1080p at 60 fps over USB-C.", 2, "about $100-170", "Best Buy, Walmart, Staples" },
	{ "Arducam global shutter USB camera (OV9281)", "ov9281", 0, 0, 0, "global-shutter",
		"A global shutter: no lean on fast cars, up to 120 fps at 1280 × 800. Field of view depends on the lens fitted.", 2, "about $50-90", "online (Arducam)" },
	// Suggested: a good start.
	{ "Logitech C922", "c922", 0x046d, 0x085c, 70, "webcam", "60 fps at 720p (MJPG), which VST picks; VST sets its shutter for you.", 1,
		"about $70-100", "Best Buy, Walmart, Staples" },
	// Recognized: worth a hint, not a suggestion.
	{ "Logitech Brio", "brio", 0x046d, 0x085e, 82, "webcam", "60 fps at 1080p and up to 90 at 720p on a USB 3 port.", 0, "", "" },
	{ "Logitech C920", "c920", 0x046d, 0x082d, 70, "webcam",
		"30 fps at 720p and 1080p. VST stops it dropping to 15 fps in shade where the system lets it.", 0, "", "" },
	{ "Logitech C930e", "c930e", 0x046d, 0x0843, 82, "webcam", "30 fps at 1080p, wide.", 0, "", "" },
	{ "Logitech C615", "c615", 0x046d, 0x082c, 67, "webcam", "30 fps at 720p and 1080p.", 0, "", "" },
	{ "Logitech C270", "c270", 0x046d, 0x0825, 49, "webcam",
		"720p at 30 fps with a narrow lens: fine to start; on a 15 ft view it times cars up to about 45 mph.", 0, "", "" },
	{ "Microsoft LifeCam HD-3000", "lifecam", 0x045e, 0x0779, 61, "webcam", "720p at 30 fps.", 0, "", "" },
	{ "Razer Kiyo Pro", "kiyo pro", 0x1532, 0x0e05, 95, "webcam", "1080p at 60 fps, very wide: cars look smaller at the same distance.", 0, "", "" },
	{ "Razer Kiyo", "kiyo", 0x1532, 0x0e03, 74, "webcam", "60 fps at 720p, 30 at 1080p.", 0, "", "" },
	{ "PlayStation Eye", "", 0x1415, 0x2000, 63, "webcam", "60 fps at 640 × 480: small pictures, but a fine short-view timer.", 0, "", "" },
	{ "Elgato Cam Link 4K", "cam link", 0x0fd9, 0x0066, 0, "capture",
		"An HDMI camera through a capture stick: it sends what the camera sends, often 1080p at 60 fps.", 0, "", "" },
	// Phones through a webcam app or driver.
	{ "iPhone (Continuity Camera)", "iphone", 0, 0, 66, "phone", "Your iPhone as a webcam: about 30 fps on automatic exposure. Recording at 60 fps and opening the video times better.", 0, "", "" },
	{ "Phone with Iriun Webcam", "iriun", 0, 0, 66, "phone", "Your phone as a webcam: about 30 fps. Set the app to its highest frame rate.", 0, "", "" },
	{ "Phone with DroidCam", "droidcam", 0, 0, 66, "phone", "Your phone as a webcam: about 30 fps. Set the app to its highest frame rate.", 0, "", "" },
	{ "Phone with Camo", "camo", 0, 0, 66, "phone", "Your phone as a webcam. Camo can send 60 fps: pick it in Camo's settings.", 0, "", "" },
	{ "Phone with EpocCam", "epoccam", 0, 0, 66, "phone", "Your phone as a webcam: about 30 fps.", 0, "", "" },
	// Built in and virtual.
	{ "Mac's built-in camera", "facetime", 0, 0, 0, "built-in", "Fine to try VST with; it faces you, so a USB webcam or your phone is what faces the street.", 0, "", "" },
	{ "Laptop's built-in camera", "integrated", 0, 0, 0, "built-in", "Fine to try VST with; it faces you, so a USB webcam or your phone is what faces the street.", 0, "", "" },
	{ "OBS Virtual Camera", "obs virtual", 0, 0, 0, "virtual",
		"A picture made by OBS, not a camera: speeds are right only if OBS passes the camera's frames through at a steady rate.", 0, "", "" },
};
}  // namespace

const std::vector<Model>& models() {
	static const std::vector<Model> sorted = [] {
		std::vector<Model> m = kModels;
		std::stable_sort(m.begin(), m.end(), [](const Model& a, const Model& b) { return a.suggest > b.suggest; });
		return m;
	}();
	return sorted;
}

const Model* recognize(const std::string& deviceName, int vendor, int product) {
	if (vendor && product)
		for (const Model& m : kModels) if (m.vendor == vendor && m.product == product) return &m;
	std::vector<std::string> have = words(deviceName);
	const Model* found = nullptr;
	size_t most = 0;
	for (const Model& m : kModels) {
		std::vector<std::string> need = words(m.match);
		if (need.empty()) continue;
		bool all = std::all_of(need.begin(), need.end(), [&](const std::string& w) { return std::find(have.begin(), have.end(), w) != have.end(); });
		if (all && need.size() > most) { found = &m; most = need.size(); }   // "kiyo pro" over "kiyo"
	}
	return found;
}

const Model* recognize(const Device& d) { return recognize(d.name, d.vendor, d.product); }

}  // namespace webcam
