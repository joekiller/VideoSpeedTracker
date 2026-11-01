// Local HTTP API for the VST companion app (docs/companion-api.md is the shared contract).
//
// Off until the user turns it on.  Listens on all interfaces at the chosen port (8787 by default) so a phone on
// the same Wi-Fi can reach it; every /api route except /api/hello needs the pairing token, as
// "Authorization: Bearer <token>" or "?token=<token>" (for <img>/<video> sources).  The token travels in the
// URL fragment of the pairing QR code, http://<lan-ip>:<port>/#token=<token>, so it never reaches server logs.
//
// Hardening for a box left running for weeks:
//   * The token the handlers check is the Server's own copy (setToken), so a "New pairing code" on the GUI
//     thread never races a worker reading it, and every open stream re-checks its token on each tick: a reset
//     ends live views and MJPEG streams within a second instead of leaving a signed-out phone watching.
//   * At most kMaxStreams long-lived responses (/api/stream.mjpg, /api/live) are open at once.  Each holds a
//     worker thread for as long as it's open and cpp-httplib's pool has 8 or more, so the cap keeps the rest of
//     the API answering however many tabs are left open; the next stream gets 503 with a JSON error.
//   * The address in the pairing QR code is lanAddress()'s guess unless the user picked one of
//     lanAddresses() (AppPrefs::remoteAddress), for machines with Wi-Fi plus Ethernet, a VPN or Docker bridges.
//
// Handlers run on cpp-httplib's worker threads.  Anything that reads or changes App state is handed to the GUI
// thread with App::post(); tracker data (vehicles, frames) is read directly, since Tracker is thread-safe.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class App;
namespace httplib { class Server; }

class Server {
public:
	explicit Server(App& app);
	~Server();
	// localOnly listens on 127.0.0.1 only: Pick your spot's map in this computer's browser, with the companion off.
	bool start(int port, bool localOnly = false);
	bool running() const { return running_; }
	const std::string& error() const { return error_; }
	std::string baseUrl() const;       // http://<lan-ip>:<port>/
	std::string pairingUrl() const;    // baseUrl + "#token=..."
	void drawQr(const std::string& text, float size);   // ImGui widget
	void setToken(const std::string& token);             // after a new pairing code; ends streams opened with the old one
	bool tokenMatches(const std::string& presented) const;
	void setAddress(const std::string& ip);              // "" = lanAddress()'s guess
	int openStreams() const { return streams_; }
	static constexpr int kMaxStreams = 4;
private:
	void routes();
	App& app_;
	std::unique_ptr<httplib::Server> http_;
	std::thread thread_;
	int port_ = 0;
	bool running_ = false;
	std::string error_;
	std::string ip_;
	mutable std::mutex tokenMu_;
	std::string token_;
	std::atomic<int> streams_{ 0 };
};

// Best guess at this machine's address on the local network (the interface used for outbound traffic).
std::string lanAddress();
// Every IPv4 address of this machine's interfaces that's up, loopback and link-local (169.254/16) left out,
// sorted with lanAddress()'s guess first.
std::vector<std::string> lanAddresses();
