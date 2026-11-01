// The frame input port (src/Wintel64/VideoSpeedTracker/FrameInput.h).
//
//   frame_input_test <repo root> <scratch dir>
//
// Checks each piece the engine and VST Studio now share:
//   - classify(): every kind of source name, including the ones easy to get wrong (a URL ending in .jpg, a numbered
//     image sequence, "picam:0?fps=60", a 4-digit name that is a file, not a camera)
//   - safeName() / label(): passwords never reach the log or the UI, in either place a URL can carry one
//   - the adapters through the port alone: the 2016 sample through CaptureSource (facts, seeking, the working frame),
//     a photo through ImageSource (one frame, then the end), a missing file and an unreadable photo (closed, with a
//     reason for the photo), FeedSource (each pushed frame read once, never blocking), and closed()
//   - readWorking(): any delivered size comes out 1280x720, turned by the level angle
#include "FrameInput.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cstdio>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using frameio::Kind;

static int failures = 0, checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void names() {
	CHECK(frameio::classify("") == Kind::None);
	CHECK(frameio::classify("0") == Kind::Camera);
	CHECK(frameio::classify("12") == Kind::Camera);
	CHECK(frameio::classify("2016") == Kind::File);                 // four digits: a file called 2016, not camera 2016
	CHECK(frameio::classify("picam") == Kind::PiCamera);
	CHECK(frameio::classify("picam:0?fps=60&shutter=1000") == Kind::PiCamera);
	CHECK(frameio::classify("rtsp://cam/stream") == Kind::Stream);
	CHECK(frameio::classify("http://cam/snapshot.jpg") == Kind::Stream);   // a camera's snapshot URL, not a photo on disk
	CHECK(frameio::classify("/videos/street.MP4") == Kind::File);
	CHECK(frameio::classify("C:\\videos\\view.JPG") == Kind::Image);
	CHECK(frameio::classify("shots/img_%04d.jpg") == Kind::File);   // a numbered sequence VideoCapture plays as video
	CHECK(frameio::classify("dir.jpg/clip") == Kind::File);         // the extension is the file's, not a folder's
	CHECK(frameio::isLive(Kind::Camera) && frameio::isLive(Kind::PiCamera) && frameio::isLive(Kind::Stream));
	CHECK(!frameio::isLive(Kind::File) && !frameio::isLive(Kind::Image) && !frameio::isLive(Kind::Feed));
	CHECK(frameio::isCamera(Kind::PiCamera) && !frameio::isCamera(Kind::Stream));

	CHECK(frameio::safeName("rtsp://admin:s3cret@10.0.0.5:554/h264") == "rtsp://10.0.0.5:554/h264");
	CHECK(frameio::safeName("rtsp://admin:p@ss/word@cam/live") == "rtsp://cam/live");   // the last '@' ends the user info
	CHECK(frameio::safeName("http://cam/video.cgi?user=admin&pwd=s3cret&res=1") == "http://cam/video.cgi?user=admin&res=1");
	CHECK(frameio::safeName("http://cam/v?Password=x#frag") == "http://cam/v#frag");
	CHECK(frameio::safeName("/home/joe/a@b.mp4") == "/home/joe/a@b.mp4");
	CHECK(frameio::label("0") == "camera 0");
	CHECK(frameio::label("rtsp://u:p@cam/x?pw=1") == "rtsp://cam/x");
	CHECK(frameio::label("picam:1").find("Pi camera 1") != std::string::npos);
}

static void sample(const fs::path& root) {
	fs::path avi = root / "sampleData" / "IPCam" / "20160130" / "manual_20160130172251.avi";
	auto src = frameio::open(avi.string());
	CHECK(src->kind() == Kind::File);
	CHECK(src->isOpened());
	frameio::Facts f = src->facts();
	CHECK(f.width == 1280 && f.height == 720);
	CHECK(f.fps > 29 && f.fps < 31);
	CHECK(f.frames > 15000);
	cv::Mat a, b;
	CHECK(frameio::readWorking(*src, a, 0) && a.cols == 1280 && a.rows == 720 && a.type() == CV_8UC3);
	CHECK(src->set(cv::CAP_PROP_POS_FRAMES, 100));   // files seek through the port
	CHECK(frameio::readWorking(*src, b, 3) && b.size() == a.size());
	CHECK(b.at<cv::Vec3b>(0, 0) == cv::Vec3b(0, 0, 0));   // turned by 3 degrees: the corners go black (Level.h)
	src->release();
	CHECK(!src->isOpened());
	CHECK(!src->read(a));
	src->release();   // twice is fine

	auto missing = frameio::open((root / "no-such-video.mp4").string());
	CHECK(missing && !missing->isOpened());
}

static void photo(const fs::path& scratch) {
	fs::create_directories(scratch);
	std::string jpg = (scratch / "view.jpg").string();
	cv::imwrite(jpg, cv::Mat(480, 640, CV_8UC3, cv::Scalar(10, 200, 30)));
	auto src = frameio::open(jpg);
	CHECK(src->kind() == Kind::Image && src->isOpened());
	CHECK(src->facts().width == 640 && src->facts().height == 480 && src->facts().fps == 0);
	cv::Mat f;
	CHECK(frameio::readWorking(*src, f, 0) && f.cols == 1280 && f.rows == 720);   // scaled up to the working size
	CHECK(!src->read(f));   // one frame, then the end

	std::string bad = (scratch / "not-a-photo.png").string();
	{ FILE* fp = std::fopen(bad.c_str(), "w"); if (fp) { std::fputs("text", fp); std::fclose(fp); } }
	auto broken = frameio::open(bad);
	CHECK(!broken->isOpened());
	CHECK(broken->error().find("Can't read image") == 0);
}

static void feed() {
	frameio::FeedSource in({ 1920, 1080, 15, 0, "MJPG" });
	CHECK(in.kind() == Kind::Feed && in.isOpened());
	cv::Mat f;
	CHECK(!in.read(f));   // nothing pushed yet: no wait, just no frame
	cv::Mat pushed(720, 1280, CV_8UC3, cv::Scalar::all(7));
	in.push(pushed);
	CHECK(in.read(f) && f.data == pushed.data);   // shared, not copied
	CHECK(!in.read(f));   // each frame once
	in.push(cv::Mat(360, 640, CV_8UC3, cv::Scalar::all(9)));
	CHECK(frameio::readWorking(in, f, 0) && f.cols == 1280 && f.at<cv::Vec3b>(10, 10)[0] == 9);
	frameio::Facts facts = in.facts();
	CHECK(facts.width == 1920 && facts.height == 1080 && facts.fps == 15);
	in.release();
	in.push(pushed);
	CHECK(!in.isOpened() && !in.read(f));

	auto none = frameio::closed();
	CHECK(none->kind() == Kind::None && !none->isOpened() && !none->read(f));
	CHECK(frameio::open("")->kind() == Kind::None);
}

int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: frame_input_test <repo root> <scratch dir>\n"); return 2; }
	names();
	sample(argv[1]);
	photo(argv[2]);
	feed();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
