// USB cameras as the system lists them (src/Wintel64/VideoSpeedTracker/Webcam.h), without a camera.
//
//   webcam_test
//
// Checks:
//   - best(): the mode VST opens, from mode lists as real drivers give them (a C922, a C920, a C270, a Facecam, a
//     PlayStation Eye, a Brio, a phone app, a camera that only lists H.264 or Bayer, nothing at all)
//   - offered(): one entry per size and rate, the better format of two, best first
//   - Settings in the source name: "0" stays "0" for everyone who never changes anything; settings round-trip; unknown
//     keys are ignored; classify() and deviceKey() (FrameInput.h) treat "0?w=1920&h=1080" as camera 0, the same device
//   - changeNote(): what a change means for marks and calibration, and that a rate or shutter change never alarms
//   - recognize(): USB ids first, then whole words ("Kiyo Pro" isn't a Kiyo); unknown cameras are null, which is fine
//   - list(): runs on a machine with no camera and returns nothing, quickly
#include "Webcam.h"
#include "FrameInput.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

using webcam::Mode;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool is(const Mode& m, int w, int h, double fps, const char* fmt) {
	bool ok = m.width == w && m.height == h && std::lround(m.fps) == std::lround(fps) && m.format == fmt;
	if (!ok) std::printf("  got %s\n", m.text().c_str());
	return ok;
}

static void bestMode() {
	// Logitech C922 on Linux: raw YUYV is slow at HD over USB 2; MJPG does 720p60.
	std::vector<Mode> c922 = {
		{ 640, 480, 30, "YUYV" }, { 1280, 720, 10, "YUYV" }, { 1920, 1080, 5, "YUYV" },
		{ 640, 480, 30, "MJPG" }, { 1280, 720, 60, "MJPG" }, { 1280, 720, 30, "MJPG" }, { 1920, 1080, 30, "MJPG" }, { 1920, 1080, 24, "MJPG" },
	};
	CHECK(is(webcam::best(c922), 1280, 720, 60, "MJPG"));
	CHECK(webcam::whyBest(webcam::best(c922), c922).find("most frames per second") != std::string::npos);
	// C920: 30 fps at most; 720p before 1080p (the working frame is 720p), MJPG before raw at the same rate.
	std::vector<Mode> c920 = { { 640, 480, 30, "YUYV" }, { 1280, 720, 30, "YUYV" }, { 1920, 1080, 30, "MJPG" }, { 1280, 720, 30, "MJPG" }, { 2304, 1536, 2, "YUYV" } };
	CHECK(is(webcam::best(c920), 1280, 720, 30, "MJPG"));
	CHECK(webcam::whyBest(webcam::best(c920), c920).find("at most 30 fps") != std::string::npos);
	// C270: 4:3 sizes and one 16:9 at 720p; the 16:9 wins over a bigger 4:3 at the same rate.
	std::vector<Mode> c270 = { { 640, 480, 30, "YUYV" }, { 960, 720, 30, "MJPG" }, { 1280, 720, 30, "MJPG" }, { 1280, 960, 30, "MJPG" } };
	CHECK(is(webcam::best(c270), 1280, 720, 30, "MJPG"));
	// Facecam (uncompressed over USB 3): 1080p60 and 720p60, NV12 and YUYV; 720p60 is enough, and nothing is MJPG.
	std::vector<Mode> facecam = { { 1920, 1080, 60, "NV12" }, { 1920, 1080, 60, "YUYV" }, { 1280, 720, 60, "YUYV" }, { 1280, 720, 30, "YUYV" } };
	CHECK(is(webcam::best(facecam), 1280, 720, 60, "YUYV"));
	// PlayStation Eye: 60 fps only at 640x480 (and 187 at 320x240, too small): 60 at 480p beats 30 at nothing bigger.
	std::vector<Mode> eye = { { 640, 480, 60, "YUYV" }, { 640, 480, 30, "YUYV" }, { 320, 240, 187, "YUYV" } };
	CHECK(is(webcam::best(eye), 640, 480, 60, "YUYV"));
	// A cheap 1080p30 camera that also does 480p60: frames win, it's class 4 against class 3.
	std::vector<Mode> cheap = { { 1920, 1080, 30, "MJPG" }, { 1280, 720, 30, "MJPG" }, { 640, 480, 60, "MJPG" } };
	CHECK(is(webcam::best(cheap), 640, 480, 60, "MJPG"));
	CHECK(webcam::whyBest(webcam::best(cheap), cheap).find("only below HD") != std::string::npos);
	// Brio: 90 at 720p beats 60; 4K costs decoding for nothing.
	std::vector<Mode> brio = { { 3840, 2160, 30, "MJPG" }, { 1920, 1080, 60, "MJPG" }, { 1280, 720, 90, "MJPG" }, { 1280, 720, 60, "MJPG" } };
	CHECK(is(webcam::best(brio), 1280, 720, 90, "MJPG"));
	// 59.94 and 60 are the same rate; then the nearer height wins.
	std::vector<Mode> ntsc = { { 1920, 1080, 60, "MJPG" }, { 1280, 720, 59.94, "MJPG" } };
	CHECK(is(webcam::best(ntsc), 1280, 720, 59.94, "MJPG"));
	// 240 fps at 720p is capped at 120 for ranking, so a 120 fps mode at 720p ties and the format decides.
	std::vector<Mode> fast = { { 1280, 720, 240, "YUYV" }, { 1280, 720, 120, "MJPG" } };
	CHECK(is(webcam::best(fast), 1280, 720, 120, "MJPG"));
	// H.264 and Bayer aren't pictures VideoCapture gives; a camera listing only those has no best mode.
	std::vector<Mode> h264 = { { 1920, 1080, 30, "H264" }, { 1280, 720, 60, "H264" } };
	CHECK(!webcam::best(h264).valid());
	CHECK(webcam::whyBest(webcam::best(h264), h264).find("None of the modes") != std::string::npos);
	std::vector<Mode> bayer = { { 1456, 1088, 60, "pRAA" } };
	CHECK(!webcam::best(bayer).valid());
	CHECK(!webcam::best({}).valid());
	CHECK(webcam::whyBest(Mode(), {}).find("asks it for 1280") != std::string::npos);
	// A format VST has never seen is tried, not dropped.
	CHECK(Mode({ 1280, 720, 30, "XYZW" }).usable());
	CHECK(Mode({ 1920, 1080, 30, "MJPG" }).wide() && !Mode({ 640, 480, 30, "MJPG" }).wide() && Mode({ 848, 480, 30, "MJPG" }).wide());
	CHECK(Mode({ 1280, 720, 60, "MJPG" }).text() == "1280 × 720 at 60 fps (MJPG)");
}

static void offeredModes() {
	std::vector<Mode> c922 = {
		{ 640, 480, 30, "YUYV" }, { 1280, 720, 10, "YUYV" }, { 640, 480, 30, "MJPG" }, { 1280, 720, 60, "MJPG" }, { 1280, 720, 30, "MJPG" },
		{ 1920, 1080, 30, "MJPG" }, { 1280, 720, 30, "YUYV" }, { 160, 120, 30, "YUYV" }, { 1920, 1080, 30, "H264" },
	};
	std::vector<Mode> o = webcam::offered(c922);
	CHECK(!o.empty() && is(o[0], 1280, 720, 60, "MJPG"));
	int at720x30 = 0, tiny = 0, h264 = 0;
	for (const Mode& m : o) { at720x30 += m.width == 1280 && m.fps == 30; tiny += m.width < 320; h264 += m.format == "H264"; }
	CHECK(at720x30 == 1);   // MJPG and YUYV at 720p30: one entry, MJPG
	CHECK(tiny == 0 && h264 == 0);
	CHECK(webcam::offered(c922, 2).size() == 2);
}

static void settingsInName() {
	webcam::Settings s;
	CHECK(webcam::isName("0") && webcam::isName("12") && webcam::isName("0?w=1920&h=1080"));
	CHECK(!webcam::isName("2016") && !webcam::isName("") && !webcam::isName("picam") && !webcam::isName("cam.mp4"));
	CHECK(webcam::parse("0", s) && s.camera == 0 && s.isDefault());
	CHECK(webcam::name(s) == "0");
	s = webcam::Settings();
	s.camera = 2; s.width = 1920; s.height = 1080; s.fps = 30; s.format = "MJPG"; s.shutterUs = 1000;
	std::string n = webcam::name(s);
	CHECK(n == "2?w=1920&h=1080&fps=30&fmt=MJPG&shutter=1000");
	webcam::Settings back;
	CHECK(webcam::parse(n, back) && back.camera == 2 && back.width == 1920 && back.height == 1080 && back.fps == 30 && back.format == "MJPG" && back.shutterUs == 1000);
	CHECK(webcam::parse("1?shutter=0&future=yes", back) && back.camera == 1 && back.shutterUs == 0 && back.picksMode());
	CHECK(webcam::parse("0?fps=59.94", back) && std::fabs(back.fps - 59.94) < 1e-9 && webcam::name(back) == "0?fps=59.94");
	CHECK(!webcam::parse("rtsp://x", back));

	// The frame input port reads these as the same camera.
	CHECK(frameio::classify("0?w=1920&h=1080") == frameio::Kind::Camera);
	CHECK(frameio::classify("2016") == frameio::Kind::File);
	CHECK(frameio::label("3?fps=30") == "camera 3");
	CHECK(frameio::deviceKey("0") == frameio::deviceKey("0?w=1920&h=1080&shutter=0"));
	CHECK(frameio::deviceKey("0") != frameio::deviceKey("1"));
	CHECK(frameio::deviceKey("picam:1?fps=30") == frameio::deviceKey("picam:1"));
	CHECK(frameio::deviceKey("picam:1") != frameio::deviceKey("picam:0"));
	CHECK(frameio::deviceKey("/videos/a.mp4") == "/videos/a.mp4");
}

static void changes() {
	Mode hd{ 1280, 720, 60, "MJPG" }, full{ 1920, 1080, 30, "MJPG" }, vga{ 640, 480, 30, "YUYV" }, hd30{ 1280, 720, 30, "MJPG" };
	CHECK(webcam::changeNote(hd, vga).find("16:9 to 4:3") != std::string::npos);
	CHECK(webcam::changeNote(vga, hd).find("4:3 to 16:9") != std::string::npos);
	CHECK(webcam::changeNote(hd, full).find("Only the size") != std::string::npos);
	CHECK(webcam::changeNote(hd, hd30).find("Only the frame rate") != std::string::npos);
	CHECK(webcam::changeNote(hd, hd).empty());
	CHECK(webcam::changeNote(Mode(), hd).empty());
	for (const Mode& m : { vga, full, hd30 }) CHECK(webcam::changeNote(hd, m).find("kept") != std::string::npos || webcam::changeNote(hd, m).find("still fit") != std::string::npos);
}

static void registry() {
	const webcam::Model* m = webcam::recognize("C922 Pro Stream Webcam", 0, 0);
	CHECK(m && std::string(m->name) == "Logitech C922" && m->hfov > 60);
	m = webcam::recognize("USB Camera-B4.09.24.1", 0x1415, 0x2000);   // a PlayStation Eye's name says nothing; its ids do
	CHECK(m && std::string(m->name) == "PlayStation Eye");
	m = webcam::recognize("Razer Kiyo Pro", 0, 0);
	CHECK(m && std::string(m->name) == "Razer Kiyo Pro");
	m = webcam::recognize("Razer Kiyo", 0, 0);
	CHECK(m && std::string(m->name) == "Razer Kiyo");
	CHECK(webcam::recognize("HD Pro Webcam C920", 0, 0) && webcam::recognize("Integrated_Webcam_HD", 0, 0));
	CHECK(!webcam::recognize("Some Brand 1080P Webcam", 0x1234, 0x5678));   // unknown is normal: nothing changes for it
	CHECK(!webcam::recognize("camouflage cam", 0, 0));                         // whole words: "camo" isn't in "camouflage"
	webcam::Device obs;
	obs.name = "OBS Virtual Camera";
	CHECK(obs.isVirtual());
	webcam::Device loop;
	loop.name = "Dummy video device (0x0000)";
	loop.id = "platform:v4l2loopback-000 v4l2loopback";
	CHECK(loop.isVirtual());
	webcam::Device real;
	real.name = "C922 Pro Stream Webcam";
	real.modes = { { 1280, 720, 60, "MJPG" }, { 640, 480, 90, "MJPG" } };
	CHECK(!real.isVirtual() && real.topFps() == 60 && real.label() == "C922 Pro Stream Webcam");
	webcam::Device unnamed;
	unnamed.index = 3;
	CHECK(unnamed.label() == "USB camera 3" && unnamed.topFps() == 0);
	const auto& all = webcam::models();
	CHECK(!all.empty() && all.front().suggest == 2);
	int suggestedWithoutWhere = 0;
	for (const auto& x : all) suggestedWithoutWhere += x.suggest > 0 && (std::string(x.cost).empty() || std::string(x.where).empty());
	CHECK(suggestedWithoutWhere == 0);   // a suggestion always says roughly what it costs and where to find one
}

static void listing() {
	auto t0 = std::chrono::steady_clock::now();
	std::vector<webcam::Device> all = webcam::list();
	double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	CHECK(s < 5);
	for (const auto& d : all) { std::printf("  found camera %d: %s, %zu modes\n", d.index, d.label().c_str(), d.modes.size()); CHECK(d.index >= 0); }
	webcam::Device none = webcam::describe(987);
	CHECK(none.index == 987 && none.name.empty() && none.modes.empty());
}

int main() {
	bestMode();
	offeredModes();
	settingsInName();
	changes();
	registry();
	listing();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
