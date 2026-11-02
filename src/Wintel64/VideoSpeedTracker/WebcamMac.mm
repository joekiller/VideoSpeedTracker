// Listing cameras and their formats on macOS through AVFoundation (Webcam.h).  The order is OpenCV's: its AVFoundation
// backend (cap_avfoundation_mac.mm, OpenCV 4.x and 5.x) takes devicesWithMediaType: video, then muxed, and sorts them by
// uniqueID, so index N here is VideoCapture(N).  Listing needs no camera permission; only reading pictures does.
#include "Webcam.h"
#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#include <cstdlib>
#include <string>

namespace webcam {

namespace {

std::string text(NSString* s) { const char* c = s ? [s UTF8String] : nullptr; return c ? std::string(c) : std::string(); }

// "UVC Camera VendorID_1133 ProductID_2140" (decimal ids) -> 0x046d, 0x085c
int idAfter(const std::string& model, const char* key) {
	size_t at = model.find(key);
	return at == std::string::npos ? 0 : int(std::strtol(model.c_str() + at + std::char_traits<char>::length(key), nullptr, 10));
}

std::string fourcc(FourCharCode v) {
	std::string s;
	for (int i = 3; i >= 0; i--) s += char((v >> (8 * i)) & 0xff);
	if (s == "dmb1") return "MJPG";   // AVFoundation's name for motion JPEG
	if (s == "yuvs") return "YUYV";
	if (s == "2vuy") return "UYVY";
	return s;
}

}  // namespace

std::vector<Device> listNow() {
	std::vector<Device> out;
	@autoreleasepool {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		NSArray* devices = [[AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo]
			arrayByAddingObjectsFromArray:[AVCaptureDevice devicesWithMediaType:AVMediaTypeMuxed]];
#pragma clang diagnostic pop
		devices = [devices sortedArrayUsingComparator:^NSComparisonResult(AVCaptureDevice* a, AVCaptureDevice* b) {
			return [a.uniqueID compare:b.uniqueID];
		}];
		int index = 0;
		for (AVCaptureDevice* dev in devices) {
			Device d;
			d.index = index++;
			d.backend = "AVFOUNDATION";
			d.name = text(dev.localizedName);
			d.id = text(dev.uniqueID);
			std::string model = text(dev.modelID);
			d.vendor = idAfter(model, "VendorID_");
			d.product = idAfter(model, "ProductID_");
			for (AVCaptureDeviceFormat* f in dev.formats) {
				CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
				std::string fmt = fourcc(CMFormatDescriptionGetMediaSubType(f.formatDescription));
				for (AVFrameRateRange* r in f.videoSupportedFrameRateRanges) {
					Mode m{ int(dim.width), int(dim.height), r.maxFrameRate, fmt };
					bool seen = false;
					for (const Mode& o : d.modes) seen |= o == m;
					if (m.valid() && m.fps > 0 && !seen) d.modes.push_back(m);
				}
			}
			out.push_back(d);
		}
	}
	return out;
}

}  // namespace webcam
