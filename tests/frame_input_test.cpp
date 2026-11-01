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
//   - uvc::: a USB camera's shutter in each backend's units (V4L2 100 us steps, Windows log2 seconds, none on macOS),
//     the read-back check, the ladder of shutters tried and the brightness rule that ends it, without a camera
//   - CameraReport: the issue text a probe makes, from a hand-made report, and a probe of a camera that isn't there
#include "FrameInput.h"
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <cmath>
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

static void shutter() {
	using namespace frameio::uvc;
	CHECK(exposureValue("V4L2", 1000) == 10);          // 1/1000 s is ten 100 us steps
	CHECK(exposureValue("V4L2", 4000) == 40);
	CHECK(exposureValue("V4L2", 20) == 1);             // never 0, which some drivers read as "longest"
	CHECK(exposureValue("MSMF", 1000) == -10);         // log2(0.001) = -9.97: 1/1024 s
	CHECK(exposureValue("DSHOW", 2000) == -9);
	CHECK(exposureValue("DSHOW", 4000) == -8);
	CHECK(std::isnan(exposureValue("AVFOUNDATION", 1000)));
	CHECK(std::isnan(exposureValue("GSTREAMER", 1000)));
	CHECK(std::isnan(exposureValue("V4L2", 0)));
	CHECK(autoExposureValue("V4L2", false) == 1 && autoExposureValue("V4L2", true) == 3);
	CHECK(autoExposureValue("MSMF", false) == 0 && autoExposureValue("MSMF", true) == 1);
	CHECK(tookExposure("V4L2", 10, 10) && tookExposure("V4L2", 10, 11) && !tookExposure("V4L2", 10, 156));
	CHECK(tookExposure("V4L2", 40, 48) && !tookExposure("V4L2", 40, 60));
	CHECK(tookExposure("MSMF", -10, -10) && !tookExposure("MSMF", -10, -6) && !tookExposure("MSMF", -10, NAN));
	CHECK((ladder(1000) == std::vector<int>{ 1000, 2000, 4000 }));
	CHECK((ladder(500) == std::vector<int>{ 500, 1000, 2000 }));
	CHECK((ladder(4000) == std::vector<int>{ 4000 }));
	CHECK((ladder(8000) == std::vector<int>{ 8000 }));   // slower than 1/250 s asked for: just that
	CHECK(ladder(0).empty());
	CHECK(brightEnough(120, 70));     // daylight, a bit darker fixed: fine
	CHECK(!brightEnough(120, 45));    // under 45% of automatic
	CHECK(!brightEnough(60, 35));     // under 40 whatever automatic was
	CHECK(brightEnough(60, 41));      // a dim evening isn't held to daylight
	CHECK(!brightEnough(120, NAN));
}

static void report() {
	frameio::CameraReport r;
	r.index = 2;
	r.name = "HD Pro Webcam C922";
	r.backend = "V4L2";
	r.opened = true;
	r.modes.push_back({ 1280, 720, 60, 1280, 720, 60, 59.6 });
	r.modes.push_back({ 1920, 1080, 60, 1920, 1080, 30, 0 });
	r.controls.push_back({ "EXPOSURE", 156, true, true });
	r.controls.push_back({ "ZOOM", -1, false, false });
	r.policy.width = 1280; r.policy.height = 720; r.policy.fps = 60; r.policy.measuredFps = 59.8;
	r.policy.shutter = frameio::Shutter::Fixed; r.policy.shutterUs = 1000;
	r.notes.push_back("Camera shutter: fixed at 1/1000 s.");
	std::string md = r.markdown();
	CHECK(md.find("Camera** 2 (HD Pro Webcam C922), backend V4L2") != std::string::npos);
	CHECK(md.find("| 1280x720 @ 60 | 1280x720 | 60 fps | 60 fps |") != std::string::npos);
	CHECK(md.find("| 1920x1080 @ 60 | 1920x1080 | 30 fps | nothing |") != std::string::npos);
	CHECK(md.find("| EXPOSURE | 156 | yes | yes |") != std::string::npos);
	CHECK(md.find("| ZOOM |  | no | no |") != std::string::npos);
	CHECK(md.find("shutter fixed at 1/1000 s") != std::string::npos);
	CHECK(md.find("- Camera shutter: fixed at 1/1000 s.") != std::string::npos);

	frameio::CameraReport none = frameio::probeCamera(987);   // no such camera: closed, with a reason, quickly
	CHECK(!none.opened && !none.error.empty());
	CHECK(none.markdown().find(none.error) != std::string::npos);
}

int main(int argc, char** argv) {
	if (argc < 3) { std::printf("usage: frame_input_test <repo root> <scratch dir>\n"); return 2; }
	names();
	sample(argv[1]);
	photo(argv[2]);
	feed();
	shutter();
	report();
	std::printf("%d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
