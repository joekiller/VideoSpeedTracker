#include <httplib.h>   // first: it pulls in winsock2.h, which must precede windows.h
#include "Server.h"
#include "App.h"
#include "Settings.h"
#include "Theme.h"
#include "UserPaths.h"
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <qrcodegen.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>

using json = nlohmann::json;
namespace fs = std::filesystem;

#ifndef VST_VERSION
#define VST_VERSION "dev"
#endif

std::string lanAddress() {
	// "Connect" a UDP socket to a public address: no packet is sent, but the OS picks the outbound interface,
	// whose address getsockname() then reports.
	std::string ip = "127.0.0.1";
	auto s = socket(AF_INET, SOCK_DGRAM, 0);
#ifdef _WIN32
	if (s == INVALID_SOCKET) return ip;
#else
	if (s < 0) return ip;
#endif
	sockaddr_in dst{};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(53);
	inet_pton(AF_INET, "8.8.8.8", &dst.sin_addr);
	if (connect(s, (sockaddr*)&dst, sizeof dst) == 0) {
		sockaddr_in me{};
		socklen_t len = sizeof me;
		char buf[INET_ADDRSTRLEN] = {};
		if (getsockname(s, (sockaddr*)&me, &len) == 0 && inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf)) ip = buf;
	}
#ifdef _WIN32
	closesocket(s);
#else
	close(s);
#endif
	return ip;
}

namespace {

std::string lowerDir(const std::string& s) { return s == "L2R" ? "L2R" : s == "R2L" ? "R2L" : "both"; }

json vehicleJson(const Vehicle& v) {
	return json{
		{ "id", v.id }, { "time", v.live || v.recorded.empty() ? v.time : v.recorded }, { "video", v.video }, { "frame", v.ev.frame },
		{ "dir", v.ev.leftToRight ? "L2R" : "R2L" }, { "compass", v.ev.compass }, { "wrongWay", v.ev.wrongWay },
		{ "speed", v.ev.speed }, { "speeding", v.speeding }, { "egregious", v.egregious },
		{ "area", v.ev.area }, { "large", v.large }, { "frames", v.frames() },
		{ "startFrame", v.ev.startFrame }, { "endFrame", v.ev.endFrame }, { "fps", v.fps },
		{ "snapshot", "/api/vehicles/" + std::to_string(v.id) + "/snapshot.jpg" },
		{ "clip", v.clipPath.empty() ? json(nullptr) : json("/api/vehicles/" + std::to_string(v.id) + "/clip.mp4") },
	};
}

// The settings the companion can read and change: VST.cfg keys by their own names, plus the app's run settings.
json configJson(App& app) {
	const Globals& g = app.cfg.g;
	return json{
		{ "AnalysisBoxLeft", g.AnalysisBoxLeft }, { "AnalysisBoxTop", g.AnalysisBoxTop },
		{ "AnalysisBoxWidth", g.AnalysisBoxWidth }, { "AnalysisBoxHeight", g.AnalysisBoxHeight },
		{ "speedLineLeft", g.speedLineLeft }, { "speedLineRight", g.speedLineRight },
		{ "obstruction", { g.obstruction[0], g.obstruction[1] } }, { "obstruction_extent", g.obstruction_extent },
		{ "R2LStreetY", g.R2LStreetY }, { "L2RStreetY", g.L2RStreetY },
		{ "L2RDirection", g.L2RDirection }, { "R2LDirection", g.R2LDirection },
		{ "legalDirection", lowerDir(g.LegalDirection) },
		{ "CalibrationFramesL2R", g.CalibrationFramesL2R }, { "CalibrationFramesR2L", g.CalibrationFramesR2L },
		{ "CalibrationSpeed", g.CalibrationSpeed }, { "CalibrationFPS", g.CalibrationFPS },
		{ "SENSITIVITY_VALUE", g.SENSITIVITY_VALUE }, { "BLUR_SIZE", g.BLUR_SIZE }, { "largeVehicleArea", g.largeVehicleArea },
		{ "speedLimit", app.prefs.speedLimit }, { "egregious", app.prefs.egregious }, { "siteName", app.prefs.siteName },
		{ "frameWidth", 1280 }, { "frameHeight", 720 },
	};
}

// Per-direction calibration standing, from calibration-history.csv (App::calibrationState).
json calibrationJson(App& app) {
	auto one = [&app](bool l2r) {
		CalState s = app.calibrationState(l2r);
		bool checked = s.kind == CalState::Verified || s.kind == CalState::Failed;
		return json{ { "state", s.apiName() }, { "error", checked ? json(s.error) : json(nullptr) },
			{ "applied", s.applied.empty() ? json(nullptr) : json(s.applied) }, { "checked", checked ? json(s.checked) : json(nullptr) },
			{ "frames", l2r ? app.cfg.g.CalibrationFramesL2R : app.cfg.g.CalibrationFramesR2L } };
	};
	return json{ { "L2R", one(true) }, { "R2L", one(false) } };
}

json statusJson(App& app) {
	auto prog = app.tracker.progress();
	Counts c = app.tracker.counts();
	const SourceInfo& in = app.player.info();
	SourceInfo::Kind k = classifySource(app.source);
	json src = app.source.empty() ? json(nullptr) : json{
		{ "kind", k == SourceInfo::Camera ? "camera" : k == SourceInfo::Stream ? "stream" : k == SourceInfo::Image ? "image" : "file" },
		{ "name", displayName(app.source) }, { "fps", in.fps }, { "width", in.width }, { "height", in.height } };
	return json{
		{ "version", VST_VERSION }, { "siteName", app.prefs.siteName },
		{ "running", app.tracker.running() }, { "paused", app.tracker.paused() },
		{ "reconnecting", app.tracker.running() && prog.reconnecting },
		{ "source", src }, { "frame", app.tracker.running() ? prog.frame : std::max(app.player.position(), 0) },
		{ "totalFrames", app.tracker.running() ? prog.totalFrames : in.frames }, { "procFps", prog.procFps },
		{ "batch", { { "index", prog.job }, { "count", prog.jobs } } },
		{ "counts", { { "vehicles", c.vehicles }, { "speeders", c.speeders }, { "egregious", c.egregious },
			{ "wrongWay", c.wrongWay }, { "topSpeed", c.topSpeed }, { "avgSpeed", c.avgSpeed } } },
		{ "speedLimit", app.prefs.speedLimit }, { "egregious", app.prefs.egregious },
		{ "legalDirection", lowerDir(app.cfg.g.LegalDirection) },
		{ "calibrationMarks", { { "L2R", { app.calibMarks[0][0], app.calibMarks[0][1] } }, { "R2L", { app.calibMarks[1][0], app.calibMarks[1][1] } } } },
		{ "calibration", calibrationJson(app) },
	};
}

// Applies a partial config object; returns the problems (empty = applied).  Runs on the GUI thread.
std::vector<std::string> applyConfig(App& app, const json& in) {
	Globals g(app.cfg.g);
	std::vector<std::string> problems;
	auto setInt = [&](const char* k, int& f) { if (in.contains(k)) { if (in[k].is_number()) f = in[k].get<int>(); else problems.push_back(std::string(k) + " must be a number"); } };
	auto setDbl = [&](const char* k, double& f) { if (in.contains(k)) { if (in[k].is_number()) f = in[k].get<double>(); else problems.push_back(std::string(k) + " must be a number"); } };
	auto setStr = [&](const char* k, std::string& f) { if (in.contains(k)) { if (in[k].is_string()) f = in[k].get<std::string>(); else problems.push_back(std::string(k) + " must be a string"); } };
	setInt("AnalysisBoxLeft", g.AnalysisBoxLeft); setInt("AnalysisBoxTop", g.AnalysisBoxTop);
	setInt("AnalysisBoxWidth", g.AnalysisBoxWidth); setInt("AnalysisBoxHeight", g.AnalysisBoxHeight);
	setInt("speedLineLeft", g.speedLineLeft); setInt("speedLineRight", g.speedLineRight);
	setInt("obstruction_extent", g.obstruction_extent); setInt("R2LStreetY", g.R2LStreetY); setInt("L2RStreetY", g.L2RStreetY);
	setInt("CalibrationFramesL2R", g.CalibrationFramesL2R); setInt("CalibrationFramesR2L", g.CalibrationFramesR2L);
	setDbl("CalibrationSpeed", g.CalibrationSpeed); setDbl("CalibrationFPS", g.CalibrationFPS);
	setInt("SENSITIVITY_VALUE", g.SENSITIVITY_VALUE); setInt("BLUR_SIZE", g.BLUR_SIZE); setInt("largeVehicleArea", g.largeVehicleArea);
	setStr("L2RDirection", g.L2RDirection); setStr("R2LDirection", g.R2LDirection);
	if (in.contains("obstruction")) {
		if (in["obstruction"].is_array() && in["obstruction"].size() == 2 && in["obstruction"][0].is_number() && in["obstruction"][1].is_number()) {
			g.obstruction[0] = in["obstruction"][0].get<int>();
			g.obstruction[1] = in["obstruction"][1].get<int>();
		}
		else problems.push_back("obstruction must be [left, right]");
	}
	if (in.contains("legalDirection")) {
		std::string d = in["legalDirection"].is_string() ? in["legalDirection"].get<std::string>() : "";
		g.LegalDirection = d == "both" ? "Both" : d;
	}
	int limit = app.prefs.speedLimit, egregious = app.prefs.egregious;
	std::string site = app.prefs.siteName;
	setInt("speedLimit", limit);
	setInt("egregious", egregious);
	setStr("siteName", site);
	if (limit < 5 || limit > 80) problems.push_back("speedLimit must be between 5 and 80");
	for (auto& p : g.problems()) problems.push_back(p);
	if (!problems.empty()) return problems;
	// A run in progress keeps the settings it started with; changes apply from the next run.
	app.cfg.g.~Globals();
	new (&app.cfg.g) Globals(g);
	app.prefs.speedLimit = limit;
	app.prefs.egregious = std::max(egregious, limit);
	app.prefs.siteName = site;
	app.prefs.save();
	if (app.cfg.dirty() && !app.cfg.save()) problems.push_back(app.cfg.error);
	return problems;
}

// Draws the zones and live boxes onto a frame for remote aiming.
void drawOverlay(cv::Mat& f, const Globals& g, const cv::Rect& ab, const std::vector<vst::TrackBox>& boxes) {
	cv::Rect box(g.AnalysisBoxLeft, g.AnalysisBoxTop, g.AnalysisBoxWidth, g.AnalysisBoxHeight);
	cv::rectangle(f, box, cv::Scalar(238, 211, 34), 2);
	for (int x : { g.speedLineLeft, g.speedLineRight })
		cv::line(f, { box.x + x, box.y }, { box.x + x, box.y + box.height }, cv::Scalar(255, 255, 255), 2);
	if (g.obstruction[0] < g.obstruction[1])
		for (int x : { g.obstruction[0], g.obstruction[1] })
			cv::line(f, { box.x + x, box.y }, { box.x + x, box.y + box.height }, cv::Scalar(21, 204, 250), 2);
	cv::line(f, { box.x, box.y + g.R2LStreetY }, { box.x + box.width, box.y + g.R2LStreetY }, cv::Scalar(61, 138, 255), 2);
	cv::line(f, { box.x, box.y + g.L2RStreetY }, { box.x + box.width, box.y + g.L2RStreetY }, cv::Scalar(255, 108, 176), 2);
	for (const auto& t : boxes) {
		cv::Rect r = t.box + ab.tl();
		cv::Scalar col = g.isWrongWay(t.leftToRight) ? cv::Scalar(94, 63, 244) : cv::Scalar(94, 197, 34);
		cv::rectangle(f, r, col, 2);
		if (t.speed > 0) cv::putText(f, std::to_string(t.speed) + " mph", { r.x, std::max(20, r.y - 8) }, cv::FONT_HERSHEY_SIMPLEX, .8, col, 2);
	}
}

}  // namespace

Server::Server(App& app) : app_(app) {}

Server::~Server() {
	if (http_) http_->stop();
	if (thread_.joinable()) thread_.join();
}

std::string Server::baseUrl() const { return "http://" + ip_ + ":" + std::to_string(port_) + "/"; }
std::string Server::pairingUrl() const { return baseUrl() + "#token=" + app_.prefs.remoteToken; }

bool Server::start(int port) {
	port_ = port;
	ip_ = lanAddress();
	http_ = std::make_unique<httplib::Server>();
	routes();
	if (!http_->bind_to_port("0.0.0.0", port)) {
		error_ = "the port is in use or not allowed";
		http_.reset();
		return false;
	}
	running_ = true;
	thread_ = std::thread([this] { http_->listen_after_bind(); running_ = false; });
	return true;
}

void Server::routes() {
	using httplib::Request;
	using httplib::Response;
	App& app = app_;

	auto sendJson = [](Response& res, const json& j, int status = 200) {
		res.status = status;
		res.set_content(j.dump(), "application/json");
	};
	// Sends a JSON body produced by App::post(), which returns "" when the GUI thread didn't get to it within 5 s.
	auto sendPosted = [sendJson](Response& res, const std::string& out) {
		json j = json::parse(out, nullptr, false);
		if (j.is_discarded()) sendJson(res, { { "error", "VST Studio is busy; try again" } }, 503);
		else sendJson(res, j);
	};
	// Token from the header or the query string; compared without early exit.
	auto authed = [&app](const Request& req) {
		std::string t;
		std::string h = req.get_header_value("Authorization");
		if (h.rfind("Bearer ", 0) == 0) t = h.substr(7);
		else if (req.has_param("token")) t = req.get_param_value("token");
		const std::string& want = app.prefs.remoteToken;
		if (t.size() != want.size() || want.empty()) return false;
		unsigned diff = 0;
		for (size_t i = 0; i < t.size(); i++) diff |= unsigned(t[i] ^ want[i]);
		return diff == 0;
	};
	auto guard = [authed, sendJson](auto handler) {
		return [=](const Request& req, Response& res) {
			if (!authed(req)) { sendJson(res, { { "error", "pairing token missing or wrong; scan the QR code in VST Studio again" } }, 401); return; }
			handler(req, res);
		};
	};
	auto parseBody = [](const Request& req, json& out) {
		out = json::parse(req.body, nullptr, false);
		return !out.is_discarded() && out.is_object();
	};

	http_->set_default_headers({ { "Access-Control-Allow-Origin", "*" }, { "Access-Control-Allow-Headers", "Authorization, Content-Type" },
		{ "Access-Control-Allow-Methods", "GET, PUT, POST, OPTIONS" }, { "Cache-Control", "no-store" } });
	http_->Options(R"(/api/.*)", [](const Request&, Response& res) { res.status = 204; });
	// Every error is JSON {"error": "..."} (docs/companion-api.md).  httplib calls this for any response with a
	// status of 400 or more, including ones a handler already filled in (401, 404 for a vehicle...), so only empty
	// bodies are replaced: unmatched routes, and paths the static mount point refuses (such as "/../").
	http_->set_error_handler([](const Request&, Response& res) {
		if (!res.body.empty()) return httplib::Server::HandlerResponse::Unhandled;
		const char* msg = res.status == 404 ? "not found" : res.status == 400 ? "bad request" : "request failed";
		res.set_content(json{ { "error", msg } }.dump(), "application/json");
		return httplib::Server::HandlerResponse::Handled;
	});

	http_->Get("/api/hello", [sendJson](const Request&, Response& res) {
		sendJson(res, { { "app", "VST" }, { "version", VST_VERSION }, { "auth", true } });
	});

	http_->Get("/api/status", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return statusJson(app).dump(); }));
	}));

	http_->Get("/api/vehicles", guard([&app, sendJson](const Request& req, Response& res) {
		int since = req.has_param("since") ? std::atoi(req.get_param_value("since").c_str()) : 0;
		size_t limit = req.has_param("limit") ? std::clamp(std::atoi(req.get_param_value("limit").c_str()), 1, 5000) : 100;
		std::string from = req.has_param("from") ? req.get_param_value("from") : "", to = req.has_param("to") ? req.get_param_value("to") : "";
		auto vs = app.tracker.vehiclesSince(since);
		json out = json::array();
		// Newest first; ISO local timestamps compare correctly as strings.
		for (auto it = vs.rbegin(); it != vs.rend() && out.size() < limit; ++it) {
			if (!from.empty() && it->time < from) continue;
			if (!to.empty() && it->time > to) continue;
			out.push_back(vehicleJson(*it));
		}
		sendJson(res, out);
	}));

	http_->Get(R"(/api/vehicles/(\d+))", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v)) { sendJson(res, { { "error", "no such vehicle" } }, 404); return; }
		sendJson(res, vehicleJson(v));
	}));

	http_->Get(R"(/api/vehicles/(\d+)/snapshot\.jpg)", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		std::vector<uchar> jpg;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v) || v.snapshot.empty() || !cv::imencode(".jpg", v.snapshot, jpg)) {
			sendJson(res, { { "error", "no snapshot for this vehicle" } }, 404);
			return;
		}
		res.set_content(std::string(jpg.begin(), jpg.end()), "image/jpeg");
	}));

	http_->Get(R"(/api/vehicles/(\d+)/clip\.mp4)", guard([&app, sendJson](const Request& req, Response& res) {
		Vehicle v;
		if (!app.tracker.vehicle(std::atoi(req.matches[1].str().c_str()), v) || v.clipPath.empty()) { sendJson(res, { { "error", "no clip kept for this vehicle" } }, 404); return; }
		std::ifstream in(v.clipPath, std::ios::binary);
		if (!in) { sendJson(res, { { "error", "the clip file is gone" } }, 404); return; }
		std::ostringstream buf;
		buf << in.rdbuf();
		res.set_content(buf.str(), "video/mp4");
	}));

	// Current picture: the tracker's latest frame while running, else the preview player's frame.
	auto grab = [&app](bool overlay) {
		Tracker::Live live;
		auto shared = std::make_shared<std::pair<cv::Mat, std::unique_ptr<Globals>>>();
		if (app.tracker.running() && app.tracker.latest(live, false)) shared->first = live.frame;
		// Shared state rather than references: post() gives up after 5 s and the lambda may still run later.
		std::string ok = app.post([&app, shared] {
			if (shared->first.empty() && app.player.isOpen()) shared->first = app.player.frame().clone();
			shared->second = std::make_unique<Globals>(app.cfg.g);
			return std::string("1");
		});
		cv::Mat f = shared->first.clone();
		if (!f.empty() && overlay && !ok.empty()) drawOverlay(f, *shared->second, live.analysisBox, live.boxes);
		return f;
	};

	http_->Get("/api/frame.jpg", guard([grab, sendJson](const Request& req, Response& res) {
		cv::Mat f = grab(req.has_param("overlay") && req.get_param_value("overlay") != "0");
		std::vector<uchar> jpg;
		if (f.empty() || !cv::imencode(".jpg", f, jpg, { cv::IMWRITE_JPEG_QUALITY, 80 })) { sendJson(res, { { "error", "no picture yet; open a source in VST Studio" } }, 404); return; }
		res.set_content(std::string(jpg.begin(), jpg.end()), "image/jpeg");
	}));

	http_->Get("/api/stream.mjpg", guard([grab](const Request& req, Response& res) {
		bool overlay = req.has_param("overlay") && req.get_param_value("overlay") != "0";
		res.set_chunked_content_provider("multipart/x-mixed-replace; boundary=vstframe", [grab, overlay](size_t, httplib::DataSink& sink) {
			cv::Mat f = grab(overlay);
			std::vector<uchar> jpg;
			if (!f.empty()) {
				cv::resize(f, f, cv::Size(960, 540), 0, 0, cv::INTER_AREA);
				cv::imencode(".jpg", f, jpg, { cv::IMWRITE_JPEG_QUALITY, 70 });
				std::string head = "--vstframe\r\nContent-Type: image/jpeg\r\nContent-Length: " + std::to_string(jpg.size()) + "\r\n\r\n";
				if (!sink.write(head.data(), head.size()) || !sink.write((const char*)jpg.data(), jpg.size()) || !sink.write("\r\n", 2)) return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(125));
			return sink.is_writable();
		});
	}));

	// Server-sent events: status about once a second, each vehicle as it's logged, live boxes ~5 times a second,
	// and the config when the stream opens and again whenever it changes.
	http_->Get("/api/live", guard([&app](const Request& req, Response& res) {
		struct LiveState {
			int lastVehicle = 0;    // newest vehicle id already sent
			int tick = 0;           // provider calls so far; every 5th (~1 s) sends status and checks the config
			std::string config;     // the config JSON last sent, "" before the first
		};
		auto state = std::make_shared<LiveState>();
		state->lastVehicle = req.has_param("since") ? std::atoi(req.get_param_value("since").c_str()) : app.tracker.lastVehicleId();
		res.set_chunked_content_provider("text/event-stream", [&app, state](size_t, httplib::DataSink& sink) {
			auto send = [&](const char* ev, const json& data) {
				std::string msg = std::string("event: ") + ev + "\ndata: " + data.dump() + "\n\n";
				return sink.write(msg.data(), msg.size());
			};
			if (state->tick++ % 5 == 0) {
				// One trip to the GUI thread for both bodies.  The config can change from anywhere (the desktop's
				// own editors, PUT /api/config or a calibration from any phone), so rather than hooking every
				// writer, each stream compares the config with what it last sent: a change reaches every client
				// within about a second, and the first tick always sends it.
				json both = json::parse(app.post([&app] { return json::array({ statusJson(app), configJson(app) }).dump(); }), nullptr, false);
				if (both.is_array() && both.size() == 2) {
					if (!send("status", both[0])) return false;
					std::string cfg = both[1].dump();
					if (cfg != state->config) {
						if (!send("config", both[1])) return false;
						state->config = cfg;
					}
				}
			}
			for (const Vehicle& v : app.tracker.vehiclesSince(state->lastVehicle)) {
				state->lastVehicle = v.id;
				if (!send("vehicle", vehicleJson(v))) return false;
			}
			Tracker::Live live;
			if (app.tracker.running() && app.tracker.latest(live, false)) {
				json tracks = json::array();
				int i = 0;
				for (auto& t : live.boxes)
					tracks.push_back({ { "id", i++ }, { "dir", t.leftToRight ? "L2R" : "R2L" }, { "x", live.analysisBox.x + t.box.x },
						{ "y", live.analysisBox.y + t.box.y }, { "w", t.box.width }, { "h", t.box.height }, { "speed", t.speed } });
				if (!send("tracks", tracks)) return false;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			return sink.is_writable();
		});
	}));

	http_->Get("/api/config", guard([&app, sendPosted](const Request&, Response& res) {
		sendPosted(res, app.post([&app] { return configJson(app).dump(); }));
	}));

	http_->Put("/api/config", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			auto problems = applyConfig(app, in);
			return json{ { "ok", problems.empty() }, { "problems", problems }, { "config", configJson(app) } }.dump();
		});
		// Rejected settings are a normal answer (200, ok false, the problems in plain words), not an HTTP error: the
		// companion shows the problems next to the fields, and its client treats any non-2xx as a failed request.
		sendPosted(res, out);
	}));

	http_->Post("/api/calibration/mark", guard([&app, sendJson, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
		int edge = in.value("edge", "start") == "end" ? 1 : 0;
		sendJson(res, json::parse(app.post([&app, dir, edge] {
			int frame = app.tracker.running() ? app.tracker.progress().frame : std::max(app.player.position(), 0);
			app.calibMarks[dir][edge] = frame;
			return json{ { "ok", true }, { "frame", frame }, { "marks", { app.calibMarks[dir][0], app.calibMarks[dir][1] } } }.dump();
		}), nullptr, false));
	}));

	http_->Post("/api/calibration/apply", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			double speed = in.value("speed", 0.0);
			int vid = in.value("vehicleId", 0);
			std::string err;
			if (speed <= 0) err = "speed must be > 0";
			else if (vid) {
				Vehicle v;
				if (!app.tracker.vehicle(vid, v)) err = "no vehicle " + std::to_string(vid);
				else app.applyCalibration(in.contains("dir") ? in.value("dir", "L2R") == "L2R" : v.ev.leftToRight, speed, v.frames(), v.fps);
			}
			else {
				int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
				int* m = app.calibMarks[dir];
				if (m[0] < 0 || m[1] <= m[0]) err = "mark the start and end frames first";
				else app.applyCalibration(dir == 0, speed, m[1] - m[0], app.player.info().fps);
			}
			if (err.empty() && !app.cfg.save()) err = app.cfg.error;
			return json{ { "ok", err.empty() }, { "problems", err.empty() ? json::array() : json::array({ err }) }, { "config", configJson(app) } }.dump();
		});
		sendPosted(res, out);   // 200 either way, as for PUT /api/config
	}));

	// A check pass: the same inputs as apply (a logged pass, or the hand-marked span), but nothing changes except a
	// "check" row in calibration-history.csv.  The answer says what the calibration in force makes of the pass.
	http_->Post("/api/calibration/check", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string out = app.post([&app, in] {
			double speed = in.value("speed", 0.0);
			int vid = in.value("vehicleId", 0);
			std::string err;
			CalCheck k;
			if (speed <= 0) err = "speed must be > 0";
			else if (vid) {
				Vehicle v;
				if (!app.tracker.vehicle(vid, v)) err = "no vehicle " + std::to_string(vid);
				else k = app.checkCalibration(in.contains("dir") ? in.value("dir", "L2R") == "L2R" : v.ev.leftToRight, speed, v.frames(), v.fps);
			}
			else {
				int dir = in.value("dir", "L2R") == "R2L" ? 1 : 0;
				int* m = app.calibMarks[dir];
				if (m[0] < 0 || m[1] <= m[0]) err = "mark the start and end frames first";
				else k = app.checkCalibration(dir == 0, speed, m[1] - m[0], app.player.info().fps);
			}
			if (!err.empty()) return json{ { "ok", false }, { "problems", json::array({ err }) } }.dump();
			return json{ { "ok", true }, { "problems", json::array() }, { "direction", k.l2r ? "L2R" : "R2L" },
				{ "drivenMph", k.drivenMph }, { "measuredMph", k.measuredMph }, { "error", k.error }, { "verified", k.verified } }.dump();
		});
		sendPosted(res, out);   // 200 either way, as for apply
	}));

	http_->Post("/api/run", guard([&app, sendJson, sendPosted, parseBody](const Request& req, Response& res) {
		json in;
		if (!parseBody(req, in)) { sendJson(res, { { "error", "body must be a JSON object" } }, 400); return; }
		std::string action = in.value("action", "");
		if (action != "start" && action != "stop" && action != "pause" && action != "resume") {
			sendJson(res, { { "error", "action must be start, stop, pause or resume" } }, 400);
			return;
		}
		std::string out = app.post([&app, action] {
			if (action == "start") app.startTracking();
			else if (action == "stop") app.stopTracking();
			else if (action == "pause") app.tracker.setPaused(true);
			else app.tracker.setPaused(false);
			// The status after the action, so the caller needn't ask again.  "start" can be refused (no source,
			// invalid zones); ok is still true then and status.running stays false, while the desktop shows why.
			return json{ { "ok", true }, { "status", statusJson(app) } }.dump();
		});
		sendPosted(res, out);
	}));

	http_->Get("/api/stats.csv", guard([&app, sendJson](const Request&, Response& res) {
		std::string p = app.tracker.lastStatsPath();
		std::ifstream in(p);
		if (p.empty() || !in) { sendJson(res, { { "error", "no stats yet; run the tracker first" } }, 404); return; }
		std::ostringstream buf;
		buf << in.rdbuf();
		res.set_header("Content-Disposition", "attachment; filename=\"" + fs::path(p).filename().string() + "\"");
		res.set_content(buf.str(), "text/csv");
	}));

	// The street report (Report.h) for the phone to open or share: the same file "Make report" writes, without example
	// frames (those stay an explicit choice on the desktop).  The GUI thread only copies the settings; reading the csvs
	// and building the page happen here, so a big data folder never stalls the window.
	http_->Get("/api/report.html", guard([&app, sendJson](const Request& req, Response& res) {
		std::string from = req.has_param("from") ? req.get_param_value("from") : "";
		std::string to = req.has_param("to") ? req.get_param_value("to") : "";
		int64_t day;
		for (const std::string* d : { &from, &to })
			if (!d->empty() && !report::parseDay(*d, day)) { sendJson(res, { { "error", "from and to must be dates like 2026-11-02" } }, 400); return; }
		// Shared with the posted task, which may still run after a timeout here.
		auto job = std::make_shared<std::pair<report::Input, report::Settings>>();
		std::string ok = app.post([&app, job] {
			job->first = app.reportInput(false, "", "");
			job->second = app.reportSettings();
			return std::string("ok");
		});
		if (ok.empty()) { sendJson(res, { { "error", "VST Studio is busy; try again" } }, 503); return; }
		job->second.from = from;
		job->second.to = to;
		std::string path, html, err;
		if (!report::writeReport(job->first, job->second, path, html, err)) { sendJson(res, { { "error", err } }, 500); return; }
		res.set_header("Content-Disposition", "inline; filename=\"" + fs::path(path).filename().string() + "\"");
		res.set_content(html, "text/html; charset=utf-8");
	}));

	// The companion web app itself, if installed next to the program.
	fs::path web = userpaths::executableDir() / "companion";
	std::error_code ec;
	if (!fs::is_directory(web, ec)) web = userpaths::executableDir() / ".." / "share" / "videospeedtracker" / "companion";
	if (fs::is_directory(web, ec)) http_->set_mount_point("/", web.string());
	else
		http_->Get("/", [](const Request&, Response& res) {
			res.set_content("<!doctype html><meta name=viewport content='width=device-width'><title>VST</title>"
				"<body style='font:16px system-ui;background:#0b0e14;color:#e7ebf3;padding:24px'>"
				"<h2>VST Studio is running</h2><p>The companion web app isn't installed with this copy. Its API is at <code>/api</code>.</p>",
				"text/html");
		});
}

void Server::drawQr(const std::string& text, float size) {
	using qrcodegen::QrCode;
	QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
	int n = qr.getSize(), quiet = 3;
	float cell = std::floor(size / (n + quiet * 2));
	float total = cell * (n + quiet * 2);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	ImVec2 p = ImGui::GetCursorScreenPos();
	dl->AddRectFilled(p, ImVec2(p.x + total, p.y + total), IM_COL32_WHITE, theme::px(10));
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qr.getModule(x, y)) {
				ImVec2 a(p.x + (x + quiet) * cell, p.y + (y + quiet) * cell);
				dl->AddRectFilled(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(11, 14, 20, 255));
			}
	ImGui::Dummy(ImVec2(total, total));
}
