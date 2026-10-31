// VST Studio entry point: window, OpenGL and ImGui setup, and the frame loop.
//
//   VSTStudio                       normal start
//   VSTStudio --config FILE         use this VST.cfg instead of the remembered/per-user one
//   VSTStudio --shots DIR           documentation mode: fresh preferences, the bundled sample video, a scripted
//                                   tour of every page, a PNG of each in DIR, then exit (works under Xvfb); exits
//                                   non-zero if a step timed out or no vehicles were tracked
//   VSTStudio --write-icon FILE     write the app icon as a 256x256 PNG (packaging/vst-studio.png)
#include "App.h"
#include "Icons.h"
#include "Theme.h"
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// The icon: a speedometer dial (green-amber-red arc) with the needle in the red, on the app's dark tile.
static cv::Mat makeIcon(int size) {
	int s = size * 4;   // draw large and scale down for anti-aliasing
	cv::Mat img(s, s, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	cv::rectangle(img, cv::Rect(s / 16, s / 16, s * 7 / 8, s * 7 / 8), cv::Scalar(32, 22, 17, 255), cv::FILLED, cv::LINE_AA);
	// Rounded corners: clear them and add quarter discs.
	int r = s / 5, a = s / 16, b = s - s / 16;
	cv::Mat mask(s, s, CV_8UC1, cv::Scalar(0));
	cv::rectangle(mask, cv::Rect(a + r, a, b - a - 2 * r, b - a), 255, cv::FILLED);
	cv::rectangle(mask, cv::Rect(a, a + r, b - a, b - a - 2 * r), 255, cv::FILLED);
	for (cv::Point c : { cv::Point(a + r, a + r), cv::Point(b - r, a + r), cv::Point(a + r, b - r), cv::Point(b - r, b - r) })
		cv::circle(mask, c, r, 255, cv::FILLED, cv::LINE_AA);
	cv::Mat out(s, s, CV_8UC4, cv::Scalar(0, 0, 0, 0));
	img.copyTo(out, mask);
	cv::Point c(s / 2, s * 9 / 16);
	int rad = s * 5 / 16, th = s / 14;
	cv::ellipse(out, c, { rad, rad }, 0, 150, 255, cv::Scalar(94, 197, 34, 255), th, cv::LINE_AA);
	cv::ellipse(out, c, { rad, rad }, 0, 262, 330, cv::Scalar(36, 165, 245, 255), th, cv::LINE_AA);
	cv::ellipse(out, c, { rad, rad }, 0, 337, 390, cv::Scalar(68, 68, 239, 255), th, cv::LINE_AA);
	double ang = 370 * CV_PI / 180;
	cv::Point tip(c.x + int(std::cos(ang) * rad * .82), c.y + int(std::sin(ang) * rad * .82));
	cv::line(out, c, tip, cv::Scalar(243, 235, 231, 255), s / 22, cv::LINE_AA);
	cv::circle(out, c, s / 16, cv::Scalar(243, 235, 231, 255), cv::FILLED, cv::LINE_AA);
	cv::resize(out, out, cv::Size(size, size), 0, 0, cv::INTER_AREA);
	return out;
}

static void setWindowIcon(GLFWwindow* w) {
	cv::Mat icon = makeIcon(128), rgba;
	cv::cvtColor(icon, rgba, cv::COLOR_BGRA2RGBA);
	GLFWimage img{ rgba.cols, rgba.rows, rgba.data };
	glfwSetWindowIcon(w, 1, &img);   // ignored on macOS and Wayland
}

static void savePng(int w, int h, const std::string& path) {
	cv::Mat px(h, w, CV_8UC4);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data);
	cv::flip(px, px, 0);
	cv::cvtColor(px, px, cv::COLOR_RGBA2BGR);
	cv::imwrite(path, px);
	std::printf("wrote %s\n", path.c_str());
}

// Scripted tour for --shots: each step waits for a condition (or a number of frames), sets up a page, and
// captures it.  Drives the real UI, so the screenshots show exactly what users see.
struct DemoScript {
	App& app;
	std::string dir;
	int step = 0, frames = 0;
	bool done = false;
	std::string pendingShot;
	std::vector<std::string> failures;   // steps that gave up; any of these makes --shots exit non-zero

	void shot(const char* name) { pendingShot = (fs::path(dir) / name).string(); }
	void fail(const std::string& why) { std::fprintf(stderr, "--shots: %s\n", why.c_str()); failures.push_back(why); }
	void next() { step++; frames = 0; }

	void tick() {
		frames++;
		switch (step) {
		case 0:
			app.page = Page::Guide;
			if (frames == 20) { shot("01-get-started.png"); next(); }
			break;
		case 1:
			app.guideStep_ = 1;
			if (frames == 20) { shot("02-site-planner.png"); next(); }
			break;
		case 2:
			if (frames == 1) {
				app.openSource(app.findSample());
				app.cfg.g.LegalDirection = "R2L";   // demo: treat the sample street as one-way, to show wrong-way callouts
				app.page = Page::Source;
			}
			if (frames == 20) { shot("03-source.png"); next(); }
			break;
		case 3:
			if (frames == 1) { app.page = Page::Zones; app.player.seek(2186); }
			if (frames == 20) { shot("04-zones.png"); next(); }
			break;
		case 4:
			if (frames == 1) { app.prefs.realtime = false; app.prefs.showMotion = true; app.startTracking(); }
			// Wait for a moment with a measured vehicle on screen and a few in the feed.
			if (frames > 30 && app.tracker.counts().vehicles >= 9 && !app.live_.boxes.empty()
				&& std::any_of(app.live_.boxes.begin(), app.live_.boxes.end(), [](const vst::TrackBox& b) { return b.speed > 0; })) {
				app.tracker.setPaused(true);
				next();
			}
			// Give up on the live screenshot rather than hang, but record it: the tour skips step 5 (whose retry would
			// only bring it back here) and carries on so the other pages still get captured, and main() turns any
			// failure into a non-zero exit for CI.
			else if (frames > 6000) {
				fail("timed out waiting for a measured vehicle on the Live page");
				app.stopTracking();
				step = 6;
				frames = 0;
			}
			else if (frames > 30 && !app.tracker.running()) {
				fail("tracking stopped after " + std::to_string(app.tracker.counts().vehicles) + " vehicles, before the Live screenshot");
				step = 6;
				frames = 0;
			}
			break;
		case 5:
			// The frame on screen can be a few frames newer than the one that triggered the pause; retry if it
			// no longer shows a measured vehicle.
			if (frames == 8 && !std::any_of(app.live_.boxes.begin(), app.live_.boxes.end(), [](const vst::TrackBox& b) { return b.speed > 0; })) {
				app.tracker.setPaused(false);
				step = 4;
				frames = 2;
				break;
			}
			if (frames == 10) {
				app.toasts_.clear();
				// The health card counts live cameras only, and the tour plays a recording: show it as it reads after
				// a day on a live camera (screenshot mode never writes health.csv).
				auto& h = app.health;
				h.today.date = "2026-11-02";
				h.today.trackedHours = 9.4;
				h.today.outages = 1;
				h.today.outageHours = 3 / 60.0;
				h.view.result = health::ViewCheck::Same;
				h.view.confidence = .91;
				h.viewCheckedAt = "2026-11-02T17:20:00";
				h.video = { 3435973837ull, 412 };
				h.budgetBytes = 20ull << 30;
				h.freeBytes = 195421011968ull;
			}
			if (frames == 15) { shot("05-live-tracking.png"); }
			if (frames == 20) { app.prefs.showMotion = false; app.tracker.setPaused(false); next(); }
			break;
		case 6:
			if (!app.tracker.running() && frames > 10) {
				app.page = Page::Results;
				app.showSessionResults();
				// Select the fastest vehicle for the detail panel.
				int best = 0;
				for (size_t i = 0; i < app.rows.size(); i++) if (app.rows[i].speed > app.rows[best].speed) best = int(i);
				app.selectedRow_ = app.rows.empty() ? -1 : best;   // nothing to select when a failed run logged nothing
				next();
			}
			break;
		case 7:
			if (frames == 5) app.toasts_.clear();
			if (frames == 20) { shot("06-results.png"); next(); }
			break;
		case 8:
			if (frames == 1) {
				app.page = Page::Calibrate;
				app.calibMode_ = 1;
				app.player.seek(2180);
				app.calibMarks[0][0] = 2161;
			}
			if (frames == 5) app.toasts_.clear();
			if (frames == 20) shot("07-calibrate.png");
			// Then the check pass, on the drive-through list: a right-to-left pass "driven" 5 mph slower than the
			// calibration says it was (a failed check, 07c), then a left-to-right one at what it says (verified,
			// 07b), so the Current calibration card shows both states.  --shots never writes the history file.
			if (frames == 21 || frames == 41) {
				bool l2r = frames == 41;
				app.calibMode_ = 0;
				app.calibChecking_ = true;
				const Globals& g = app.cfg.g;
				auto vs = app.tracker.vehiclesSince(0);
				for (auto it = vs.rbegin(); it != vs.rend(); ++it) {   // the newest, at the top of the list
					const Vehicle& v = *it;
					if (v.ev.leftToRight != l2r) continue;
					double says = calib::measuredMph(l2r ? g.CalibrationFramesL2R : g.CalibrationFramesR2L, v.frames(), v.fps, g.CalibrationSpeed, g.CalibrationFPS);
					app.calibPickId_ = v.id;
					app.calibSpeed_ = int(std::lround(says)) + (l2r ? 0 : 5);
					app.checkCalibration(l2r, app.calibSpeed_, v.frames(), v.fps);
					break;
				}
			}
			if (frames == 40) shot("07c-calibrate-check-failed.png");
			if (frames == 60) shot("07b-calibrate-check.png");
			// Two cones, 10 ft apart, about where the sample's right-to-left lane runs (~200 px for 10 ft there).
			if (frames == 61) {
				const Globals& g = app.cfg.g;
				app.calibChecking_ = false;
				app.lastCheck_ = CalCheck();   // the check card belongs to the shots above
				app.calibMode_ = 2;
				app.coneFt_ = 10;
				for (int i = 0; i < 2; i++) { app.coneX_[i] = float(g.AnalysisBoxLeft + g.speedLineLeft + 100 + 200 * i); app.coneY_[i] = float(g.AnalysisBoxTop + g.R2LStreetY); }
			}
			if (frames == 80) { shot("07d-calibrate-cones.png"); next(); }
			break;
		case 9:
			if (frames == 1) { app.page = Page::Companion; app.setRemote(true); }
			if (frames == 20) { shot("08-companion.png"); next(); }
			break;
		default:
			done = true;
		}
	}
};

int main(int argc, char** argv) {
	std::string shotsDir, configArg;
	for (int i = 1; i < argc; i++) {
		std::string a = argv[i];
		if (a == "--shots" && i + 1 < argc) shotsDir = argv[++i];
		else if (a == "--config" && i + 1 < argc) configArg = argv[++i];
		else if (a == "--write-icon" && i + 1 < argc) { cv::imwrite(argv[++i], makeIcon(256)); return 0; }
		else if (a == "--help" || a == "-h") {
			std::printf("Usage: VSTStudio [--config VST.cfg] [--shots DIR] [--write-icon FILE]\n");
			return 0;
		}
	}

	glfwSetErrorCallback([](int code, const char* desc) { std::fprintf(stderr, "GLFW error %d: %s\n", code, desc); });
	// Inside VSTStudio.app, GLFW would chdir into Contents/Resources, where resolveConfig() would then pick the
	// bundle's read-only VST.cfg template as the working-directory config (and autosave would break the seal).
	glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);
	if (!glfwInit()) return 1;
	// OpenGL 3.2 core works everywhere VST runs, including macOS (which needs forward-compatible core profiles).
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
	glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
	bool shots = !shotsDir.empty();
	if (shots) glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
	int winW = shots ? 1600 : 1480, winH = shots ? 980 : 920;
	GLFWwindow* window = glfwCreateWindow(winW, winH, "VST Studio", nullptr, nullptr);
	if (!window) { glfwTerminate(); return 1; }
	glfwMakeContextCurrent(window);
	glfwSwapInterval(1);
	setWindowIcon(window);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;   // layout is fixed; preferences live in vst-studio.ini
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui_ImplGlfw_InitForOpenGL(window, true);
	ImGui_ImplOpenGL3_Init("#version 150");

	// Fonts are rasterised at the monitor's content scale so text stays sharp on HiDPI screens.
	float xs = 1, ys = 1;
	glfwGetWindowContentScale(window, &xs, &ys);
#ifdef __APPLE__
	theme::setup(1.f, xs);   // points-based UI, atlas at Retina resolution
#else
	theme::setup(shots ? 1.f : xs);
#endif

	App app;
	app.screenshotMode = shots;
	app.persist = !shots;   // a documentation run must not overwrite the user's preferences
	app.init(shots, configArg);
	DemoScript script{ app, shotsDir };
	if (shots) {
		fs::create_directories(shotsDir);
		std::string data = (fs::path(shotsDir) / "data").string();
		fs::create_directories(data);
		app.cfg.g.dataPathPrefix = data;
	}

	while (!glfwWindowShouldClose(window)) {
		// Idle politely: redraw on input, or continuously while video is moving.
		if (shots || app.wantsContinuousFrames()) glfwPollEvents();
		else glfwWaitEventsTimeout(0.25);
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		// Xvfb parks the pointer mid-screen, where it would hover whatever the tour shows there and leave a tooltip in
		// the screenshot: keep the mouse away.
		if (shots) io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
		ImGui::NewFrame();
		if (shots) script.tick();
		app.frame(io.DisplaySize.x, io.DisplaySize.y);
		ImGui::Render();
		int fw, fh;
		glfwGetFramebufferSize(window, &fw, &fh);
		glViewport(0, 0, fw, fh);
		ImVec4 bg = theme::v4(theme::c::bg0);
		glClearColor(bg.x, bg.y, bg.z, 1);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		if (!script.pendingShot.empty()) { savePng(fw, fh, script.pendingShot); script.pendingShot.clear(); }
		glfwSwapBuffers(window);
		if (shots && (script.done || ImGui::GetFrameCount() > 60000)) break;   // never hang a CI job
	}

	// A documentation run doubles as a CI smoke test of the whole app, so it must fail loudly: a tour that hit the
	// frame cap, a step that timed out, or a run that tracked nothing all exit non-zero.
	if (shots) {
		if (!script.done) script.fail("the tour stopped at step " + std::to_string(script.step) + " (frame cap or window closed)");
		if (app.tracker.counts().vehicles == 0) script.fail("no vehicles were tracked");
	}
	app.shutdown();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(window);
	glfwTerminate();
	return script.failures.empty() ? 0 : 1;
}
