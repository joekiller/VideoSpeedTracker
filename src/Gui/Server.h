// Local HTTP API for the VST companion app (docs/companion-api.md is the shared contract).
//
// Off until the user turns it on.  Listens on all interfaces at the chosen port (8787 by default) so a phone on
// the same Wi-Fi can reach it; every /api route except /api/hello needs the pairing token, as
// "Authorization: Bearer <token>" or "?token=<token>" (for <img>/<video> sources).  The token travels in the
// URL fragment of the pairing QR code, http://<lan-ip>:<port>/#token=<token>, so it never reaches server logs.
//
// Handlers run on cpp-httplib's worker threads.  Anything that reads or changes App state is handed to the GUI
// thread with App::post(); tracker data (vehicles, frames) is read directly, since Tracker is thread-safe.
#pragma once
#include <memory>
#include <string>
#include <thread>

class App;
namespace httplib { class Server; }

class Server {
public:
	explicit Server(App& app);
	~Server();
	bool start(int port);
	bool running() const { return running_; }
	const std::string& error() const { return error_; }
	std::string baseUrl() const;       // http://<lan-ip>:<port>/
	std::string pairingUrl() const;    // baseUrl + "#token=..."
	void drawQr(const std::string& text, float size);   // ImGui widget
private:
	void routes();
	App& app_;
	std::unique_ptr<httplib::Server> http_;
	std::thread thread_;
	int port_ = 0;
	bool running_ = false;
	std::string error_;
	std::string ip_;
};

// Best guess at this machine's address on the local network (the interface used for outbound traffic).
std::string lanAddress();
