// Listing USB cameras and their modes on Linux (V4L2) and Windows (Media Foundation); macOS is WebcamMac.mm.  See
// Webcam.h.  Each lister numbers cameras the way OpenCV's default backend for that platform opens them:
//   Linux    /dev/videoN is VideoCapture(N) (OpenCV's V4L2 backend opens "/dev/video" + index)
//   Windows  the order of MFEnumDeviceSources(video capture), which OpenCV's MSMF backend (the default) uses too
#include "Webcam.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#endif

namespace webcam {

#if defined(__linux__)

namespace {

std::string fourccText(unsigned v) {
	std::string s;
	for (int i = 0; i < 4; i++) s += char((v >> (8 * i)) & 0xff);
	return s;
}

int xioctl(int fd, unsigned long request, void* arg) {
	int r;
	do r = ioctl(fd, request, arg); while (r == -1 && errno == EINTR);
	return r;
}

std::string readLine(const std::string& path) {
	std::ifstream in(path);
	std::string line;
	if (in) std::getline(in, line);
	return line;
}

// The USB device's ids: /sys/class/video4linux/videoN/device is the USB interface; its parent holds idVendor/idProduct.
void usbIds(int index, int& vendor, int& product) {
	std::string base = "/sys/class/video4linux/video" + std::to_string(index) + "/device/../";
	std::string v = readLine(base + "idVendor"), p = readLine(base + "idProduct");
	vendor = v.empty() ? 0 : int(std::strtol(v.c_str(), nullptr, 16));
	product = p.empty() ? 0 : int(std::strtol(p.c_str(), nullptr, 16));
}

// A name under /dev/v4l/by-id that points at /dev/videoN: made from the maker, model and serial, so it survives a
// replug into another port (by-path doesn't).  Only the capture node ("-video-index0") is wanted.
std::string stableId(int index, const std::string& bus) {
	const std::string node = "video" + std::to_string(index);
	if (DIR* dir = opendir("/dev/v4l/by-id")) {
		std::string found;
		while (dirent* e = readdir(dir)) {
			std::string link = std::string("/dev/v4l/by-id/") + e->d_name;
			char target[PATH_MAX];
			ssize_t n = readlink(link.c_str(), target, sizeof target - 1);
			if (n <= 0) continue;
			target[n] = 0;
			std::string t = target;
			if (t.size() >= node.size() && t.compare(t.size() - node.size(), node.size(), node) == 0) { found = e->d_name; break; }
		}
		closedir(dir);
		if (!found.empty()) return found;
	}
	return bus;
}

// The rates one size runs at.  A stepwise or continuous range gives its fastest rate and 30 when 30 is in it.
void addRates(int fd, unsigned fmt, int w, int h, std::vector<Mode>& out) {
	v4l2_frmivalenum iv = {};
	iv.pixel_format = fmt;
	iv.width = unsigned(w);
	iv.height = unsigned(h);
	const std::string f = fourccText(fmt);
	for (iv.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; iv.index++) {
		if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
			if (iv.discrete.numerator) out.push_back({ w, h, double(iv.discrete.denominator) / iv.discrete.numerator, f });
			continue;
		}
		const v4l2_fract lo = iv.stepwise.min, hi = iv.stepwise.max;   // intervals: min is the fastest
		double top = lo.numerator ? double(lo.denominator) / lo.numerator : 0, bottom = hi.numerator ? double(hi.denominator) / hi.numerator : 0;
		if (top > 0) out.push_back({ w, h, top, f });
		if (top > 30.5 && bottom < 29.5) out.push_back({ w, h, 30, f });
		break;
	}
}

void addModes(int fd, Device& d) {
	v4l2_fmtdesc fd_ = {};
	fd_.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (fd_.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &fd_) == 0; fd_.index++) {
		v4l2_frmsizeenum fs = {};
		fs.pixel_format = fd_.pixelformat;
		for (fs.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0; fs.index++) {
			if (fs.type == V4L2_FRMSIZE_TYPE_DISCRETE) { addRates(fd, fd_.pixelformat, int(fs.discrete.width), int(fs.discrete.height), d.modes); continue; }
			// A range (some capture cards, virtual cameras): the common sizes inside it.
			static const int sizes[][2] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 } };
			for (const auto& s : sizes)
				if (unsigned(s[0]) >= fs.stepwise.min_width && unsigned(s[0]) <= fs.stepwise.max_width
					&& unsigned(s[1]) >= fs.stepwise.min_height && unsigned(s[1]) <= fs.stepwise.max_height)
					addRates(fd, fd_.pixelformat, s[0], s[1], d.modes);
			break;
		}
	}
}

void addControls(int fd, Device& d) {
	static const struct { unsigned id; const char* name; } wanted[] = {
		{ V4L2_CID_EXPOSURE_AUTO, "exposure_auto" }, { V4L2_CID_EXPOSURE_ABSOLUTE, "exposure_time_absolute" },
		{ V4L2_CID_EXPOSURE_AUTO_PRIORITY, "exposure_auto_priority" }, { V4L2_CID_GAIN, "gain" },
		{ V4L2_CID_FOCUS_AUTO, "focus_auto" }, { V4L2_CID_FOCUS_ABSOLUTE, "focus_absolute" },
		{ V4L2_CID_POWER_LINE_FREQUENCY, "power_line_frequency" }, { V4L2_CID_BACKLIGHT_COMPENSATION, "backlight_compensation" },
		{ V4L2_CID_ZOOM_ABSOLUTE, "zoom_absolute" }, { V4L2_CID_AUTO_WHITE_BALANCE, "white_balance_auto" },
	};
	for (const auto& w : wanted) {
		v4l2_queryctrl q = {};
		q.id = w.id;
		if (xioctl(fd, VIDIOC_QUERYCTRL, &q) != 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED)) continue;
		Control c;
		c.name = w.name;
		c.min = q.minimum; c.max = q.maximum; c.step = q.step;
		v4l2_control v = {};
		v.id = w.id;
		c.value = xioctl(fd, VIDIOC_G_CTRL, &v) == 0 ? v.value : q.default_value;
		d.controls.push_back(c);
	}
}

}  // namespace

std::vector<Device> listNow() {
	std::vector<int> nodes;
	if (DIR* dir = opendir("/sys/class/video4linux")) {
		while (dirent* e = readdir(dir)) {
			std::string n = e->d_name;
			if (n.rfind("video", 0) == 0 && n.size() > 5 && std::all_of(n.begin() + 5, n.end(), ::isdigit)) nodes.push_back(std::atoi(n.c_str() + 5));
		}
		closedir(dir);
	}
	std::sort(nodes.begin(), nodes.end());
	std::vector<Device> out;
	for (int index : nodes) {
		int fd = open(("/dev/video" + std::to_string(index)).c_str(), O_RDWR | O_NONBLOCK);
		if (fd < 0) continue;
		v4l2_capability cap = {};
		if (xioctl(fd, VIDIOC_QUERYCAP, &cap) != 0) { close(fd); continue; }
		unsigned caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
		// A UVC camera has a second node for metadata, and a Raspberry Pi lists its codec and ISP nodes here too: only
		// nodes that capture video are cameras.
		if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) { close(fd); continue; }
		Device d;
		d.index = index;
		d.backend = "V4L2";
		d.name = reinterpret_cast<const char*>(cap.card);
		std::string bus = reinterpret_cast<const char*>(cap.bus_info);
		d.id = stableId(index, bus);
		if (bus.find("v4l2loopback") != std::string::npos) d.id += " v4l2loopback";
		usbIds(index, d.vendor, d.product);
		addModes(fd, d);
		addControls(fd, d);
		close(fd);
		// Raw sensor nodes (a Pi's unicam, Bayer only) aren't cameras VST can read: those go through rpicam-vid.
		if (!d.modes.empty() && std::none_of(d.modes.begin(), d.modes.end(), [](const Mode& m) { return m.usable(); })) continue;
		out.push_back(d);
	}
	return out;
}

bool holdFrameRate(int index) {
	int fd = open(("/dev/video" + std::to_string(index)).c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0) return false;
	v4l2_control c = {};
	c.id = V4L2_CID_EXPOSURE_AUTO_PRIORITY;
	c.value = 0;
	bool ok = xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
	close(fd);
	return ok;
}

#elif defined(_WIN32)

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

std::string utf8(const wchar_t* w) {
	if (!w) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) return "";
	std::string s(size_t(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
	return s;
}

std::string attribute(IMFActivate* a, const GUID& key) {
	WCHAR* text = nullptr;
	UINT32 length = 0;
	std::string s;
	if (SUCCEEDED(a->GetAllocatedString(key, &text, &length))) s = utf8(text);
	CoTaskMemFree(text);
	return s;
}

// "\\?\usb#vid_046d&pid_085c&mi_00#..." -> 0x046d, 0x085c
int hexAfter(const std::string& link, const char* key) {
	std::string lower = link;
	for (char& c : lower) c = char(tolower((unsigned char)c));
	size_t at = lower.find(key);
	return at == std::string::npos ? 0 : int(std::strtol(lower.c_str() + at + strlen(key), nullptr, 16));
}

// A media subtype as a fourcc: MJPG, YUY2 (reported as YUYV, the name the rest of VST uses), NV12, H264 are FOURCC-
// based GUIDs; the RGB ones are D3DFORMAT numbers.
std::string subtypeText(const GUID& g) {
	if (g.Data1 == 20) return "RGB3";   // D3DFMT_R8G8B8: MFVideoFormat_RGB24
	if (g.Data1 == 21 || g.Data1 == 22) return "RGB4";
	std::string s;
	for (int i = 0; i < 4; i++) s += char((g.Data1 >> (8 * i)) & 0xff);
	return s == "YUY2" ? "YUYV" : s;
}

void addModes(IMFActivate* a, Device& d) {
	IMFMediaSource* source = nullptr;
	IMFPresentationDescriptor* pd = nullptr;
	IMFStreamDescriptor* sd = nullptr;
	IMFMediaTypeHandler* handler = nullptr;
	if (FAILED(a->ActivateObject(__uuidof(IMFMediaSource), reinterpret_cast<void**>(&source))) || !source) {
		d.error = "Another app may be using it, so its modes couldn't be read.";
		return;
	}
	BOOL selected = FALSE;
	DWORD count = 0;
	if (SUCCEEDED(source->CreatePresentationDescriptor(&pd)) && SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &selected, &sd))
		&& SUCCEEDED(sd->GetMediaTypeHandler(&handler)) && SUCCEEDED(handler->GetMediaTypeCount(&count))) {
		for (DWORD i = 0; i < count; i++) {
			IMFMediaType* type = nullptr;
			if (FAILED(handler->GetMediaTypeByIndex(i, &type)) || !type) continue;
			UINT64 size = 0, rate = 0;
			GUID sub = {};
			if (SUCCEEDED(type->GetUINT64(MF_MT_FRAME_SIZE, &size)) && SUCCEEDED(type->GetUINT64(MF_MT_FRAME_RATE, &rate))
				&& SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &sub))) {
				UINT32 w = UINT32(size >> 32), h = UINT32(size & 0xffffffff), num = UINT32(rate >> 32), den = UINT32(rate & 0xffffffff);
				Mode m{ int(w), int(h), den ? double(num) / den : 0, subtypeText(sub) };
				if (m.valid() && m.fps > 0 && std::find(d.modes.begin(), d.modes.end(), m) == d.modes.end()) d.modes.push_back(m);
			}
			release(type);
		}
	}
	release(handler);
	release(sd);
	release(pd);
	source->Shutdown();
	release(source);
	a->ShutdownObject();
}

}  // namespace

std::vector<Device> listNow() {
	std::vector<Device> out;
	HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);   // RPC_E_CHANGED_MODE: already initialised another way, fine
	if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) { if (SUCCEEDED(co)) CoUninitialize(); return out; }
	IMFAttributes* attrs = nullptr;
	IMFActivate** devices = nullptr;
	UINT32 count = 0;
	if (SUCCEEDED(MFCreateAttributes(&attrs, 1))
		&& SUCCEEDED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))
		&& SUCCEEDED(MFEnumDeviceSources(attrs, &devices, &count))) {
		for (UINT32 i = 0; i < count; i++) {
			Device d;
			d.index = int(i);
			d.backend = "MSMF";
			d.name = attribute(devices[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
			d.id = attribute(devices[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
			d.vendor = hexAfter(d.id, "vid_");
			d.product = hexAfter(d.id, "pid_");
			addModes(devices[i], d);
			out.push_back(d);
			devices[i]->Release();
		}
		CoTaskMemFree(devices);
	}
	release(attrs);
	MFShutdown();
	if (SUCCEEDED(co)) CoUninitialize();
	return out;
}

// Windows has no constant-frame-rate switch through Media Foundation's capture source that OpenCV leaves alone.
bool holdFrameRate(int) { return false; }

#elif defined(__APPLE__)

// WebcamMac.mm
bool holdFrameRate(int) { return false; }

#else

std::vector<Device> listNow() { return {}; }
bool holdFrameRate(int) { return false; }

#endif

}  // namespace webcam
