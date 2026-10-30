// Embedded-engine half of the live-stream reconnect test (tests/stream_reconnect.sh starts the stream).
//
// Usage: engine_reconnect_test <stream url> <VST.cfg> <stats csv>
//
// Runs vst::run() the way VST Studio's Tracker does, on a stream that ends after a few seconds.  The engine should then
// report "reconnecting" through Hooks::onSourceStatus with waits of 1, 2, 4 s; on the third attempt this presses Stop
// (pollKey returns 27, as Tracker::pollKey does) in the middle of the 4 s wait, and run() must return 0 within a second
// instead of finishing the wait or retrying forever.
#include "Engine.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
	if (argc != 4) { std::fprintf(stderr, "usage: %s <stream url> <VST.cfg> <stats csv>\n", argv[0]); return 2; }
	std::atomic<bool> stop{ false };
	Clock::time_point stopAt;
	std::vector<vst::SourceStatus> seen;
	int frames = 0;

	vst::Hooks hooks;
	hooks.onFrame = [&](const vst::FrameInfo&) { frames++; };
	hooks.pollKey = [&] { return stop ? 27 : -1; };
	hooks.onLog = [](const std::string& line) { std::printf("  | %s\n", line.c_str()); };
	hooks.onSourceStatus = [&](const vst::SourceStatus& s) {
		seen.push_back(s);
		if (s.reconnecting && s.attempt == 3 && !stop) { stopAt = Clock::now(); stop = true; }
	};

	int rc = vst::run({ "--input", argv[1], "--config", argv[2], "--stats", argv[3] }, hooks);
	double stopSeconds = stop ? std::chrono::duration<double>(Clock::now() - stopAt).count() : -1;

	bool ok = true;
	auto fail = [&](const char* what) { std::fprintf(stderr, "FAIL: %s\n", what); ok = false; };
	if (rc != 0) fail("vst::run() didn't return 0 after Stop");
	if (frames == 0) fail("no frames were analyzed before the stream ended");
	const int waits[] = { 1, 2, 4 };
	if (seen.size() != 3) fail("expected exactly 3 reconnecting reports before Stop");
	for (size_t i = 0; i < seen.size() && i < 3; i++)
		if (!seen[i].reconnecting || seen[i].attempt != int(i + 1) || seen[i].waitSeconds != waits[i] || seen[i].message.empty())
			fail("reconnect reports aren't attempt 1, 2, 3 with waits 1, 2, 4 s");
	if (!stop) fail("never got to the third attempt");
	else if (stopSeconds > 1.0) fail("Stop during the backoff wait took more than a second to end the run");
	std::printf("frames %d, %zu reconnect reports, run() returned %d, %.2f s after Stop\n", frames, seen.size(), rc, stopSeconds);
	return ok ? 0 : 1;
}
