#include "DashCam.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace dashcam {

namespace {

constexpr double kMphPerMps = 2.2369362920544;

uint32_t be32(const unsigned char* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const unsigned char* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }

// One MP4 box: where its contents start and how long they are.
struct Box { char type[5] = {}; uint64_t at = 0, size = 0; };

// The boxes directly inside [from, from + len): each is a 32-bit size (1 = a 64-bit size follows, 0 = to the end)
// and a four-letter type.
std::vector<Box> boxes(std::ifstream& in, uint64_t from, uint64_t len) {
	std::vector<Box> out;
	uint64_t pos = from, end = from + len;
	while (pos + 8 <= end) {
		unsigned char h[16];
		in.clear();
		in.seekg(std::streamoff(pos));
		if (!in.read(reinterpret_cast<char*>(h), 8)) break;
		uint64_t size = be32(h), header = 8;
		if (size == 1) {
			if (!in.read(reinterpret_cast<char*>(h + 8), 8)) break;
			size = be64(h + 8);
			header = 16;
		}
		else if (size == 0) size = end - pos;
		if (size < header || pos + size > end) break;
		Box b;
		std::memcpy(b.type, h + 4, 4);
		b.at = pos + header;
		b.size = size - header;
		out.push_back(b);
		pos += size;
	}
	return out;
}

const Box* find(const std::vector<Box>& bs, const char* type) {
	for (const Box& b : bs) if (std::memcmp(b.type, type, 4) == 0) return &b;
	return nullptr;
}

// moov/mvhd's duration in seconds (version 0: 32-bit times and duration; version 1: 64-bit).
double movieDuration(std::ifstream& in, const Box& moov) {
	std::vector<Box> inside = boxes(in, moov.at, moov.size);
	const Box* mvhd = find(inside, "mvhd");
	if (!mvhd || mvhd->size < 32) return 0;
	unsigned char b[32];
	in.clear();
	in.seekg(std::streamoff(mvhd->at));
	if (!in.read(reinterpret_cast<char*>(b), 32)) return 0;
	uint32_t scale = b[0] == 1 ? be32(b + 20) : be32(b + 12);
	uint64_t dur = b[0] == 1 ? be64(b + 24) : be32(b + 16);
	return scale ? double(dur) / scale : 0;
}

// The RBSP of an SEI NAL: H.264 inserts 0x03 after any two zero bytes so the payload can't mimic a start code.
std::string unescape(const unsigned char* p, size_t n) {
	std::string out;
	out.reserve(n);
	int zeros = 0;
	for (size_t i = 0; i < n; i++) {
		if (zeros >= 2 && p[i] == 3) { zeros = 0; continue; }
		out.push_back(char(p[i]));
		zeros = p[i] == 0 ? zeros + 1 : 0;
	}
	return out;
}

// The protobuf inside a Tesla SEI NAL: NAL type 6, payload type 5 (user data unregistered), a size byte, a run of
// 0x42 bytes and a 0x69 marker, then the message up to the trailing stop byte (as in Tesla's sei_extractor.py).
bool protoPayload(const std::vector<unsigned char>& nal, std::string& out) {
	if (nal.size() < 5 || (nal[0] & 0x1F) != 6 || nal[1] != 5) return false;
	size_t i = 3;
	while (i < nal.size() && nal[i] == 0x42) i++;
	if (i <= 3 || i + 1 >= nal.size() || nal[i] != 0x69) return false;
	out = unescape(nal.data() + i + 1, nal.size() - i - 2);
	return true;
}

bool varint(const std::string& s, size_t& i, uint64_t& v) {
	v = 0;
	for (int shift = 0; i < s.size() && shift < 64; shift += 7) {
		unsigned char c = s[i++];
		v |= uint64_t(c & 0x7F) << shift;
		if (!(c & 0x80)) return true;
	}
	return false;
}

// Decodes the SeiMetadata fields calibration uses: gear_state (2), vehicle_speed_mps (4, float),
// latitude_deg (11, double), longitude_deg (12, double).  Every other field is skipped by its wire type.
bool decode(const std::string& s, Sample& out) {
	size_t i = 0;
	bool any = false;
	while (i < s.size()) {
		uint64_t key;
		if (!varint(s, i, key)) return false;
		int field = int(key >> 3), wire = int(key & 7);
		if (wire == 0) {
			uint64_t v;
			if (!varint(s, i, v)) return false;
			if (field == 2) out.drive = v == 1;
		}
		else if (wire == 5) {
			if (i + 4 > s.size()) return false;
			if (field == 4) { float f; std::memcpy(&f, s.data() + i, 4); out.mps = f; any = true; }
			i += 4;
		}
		else if (wire == 1) {
			if (i + 8 > s.size()) return false;
			double d;
			std::memcpy(&d, s.data() + i, 8);
			if (field == 11) out.lat = d;
			if (field == 12) out.lon = d;
			i += 8;
		}
		else if (wire == 2) {
			uint64_t n;
			if (!varint(s, i, n) || i + n > s.size()) return false;
			i += size_t(n);
		}
		else return false;
	}
	// proto3 leaves out zero values, so a stopped car's message has no speed field; it's still a sample.
	return any || i == s.size();
}

double localEpoch(int y, int mo, int d, int h, int mi, int s) {
	std::tm tm{};
	tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
	tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
	tm.tm_isdst = -1;
	std::time_t t = std::mktime(&tm);
	return t == std::time_t(-1) ? 0 : double(t);
}

}  // namespace

double startFromName(const std::string& fileName) {
	int y, mo, d, h, mi, s;
	char tail;
	if (std::sscanf(fileName.c_str(), "%4d-%2d-%2d_%2d-%2d-%2d%c", &y, &mo, &d, &h, &mi, &s, &tail) < 6) return 0;
	return localEpoch(y, mo, d, h, mi, s);
}

double parseIso(const std::string& iso) {
	int y, mo, d, h, mi, s;
	if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
	return localEpoch(y, mo, d, h, mi, s);
}

bool readClip(const std::string& path, Clip& clip, std::string& err) {
	clip = Clip();
	clip.path = path;
	std::string name = std::filesystem::u8path(path).filename().u8string();
	std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
	if (!in) { err = "Can't open " + name + "."; return false; }
	in.seekg(0, std::ios::end);
	uint64_t len = uint64_t(in.tellg());
	std::vector<Box> top = boxes(in, 0, len);
	const Box* mdat = find(top, "mdat");
	const Box* moov = find(top, "moov");
	if (!mdat || !moov) { err = name + " isn't an MP4 video from a dashcam."; return false; }
	clip.duration = movieDuration(in, *moov);

	// mdat holds every NAL unit of the video, each prefixed with its 32-bit length.  Only SEI units are read in full;
	// the rest (the pictures) are skipped by seeking.
	std::vector<Sample> raw;
	std::vector<unsigned char> nal;
	std::string payload;
	uint64_t pos = mdat->at, end = mdat->at + mdat->size;
	while (pos + 6 <= end) {
		unsigned char h[6];
		in.clear();
		in.seekg(std::streamoff(pos));
		if (!in.read(reinterpret_cast<char*>(h), 6)) break;
		uint64_t n = be32(h);
		if (n < 2 || pos + 4 + n > end) { pos += 4 + n; continue; }
		if ((h[4] & 0x1F) == 6 && h[5] == 5) {
			nal.resize(size_t(n));
			in.seekg(std::streamoff(pos + 4));
			if (!in.read(reinterpret_cast<char*>(nal.data()), std::streamsize(n))) break;
			Sample s;
			if (protoPayload(nal, payload) && decode(payload, s)) raw.push_back(s);
		}
		pos += 4 + n;
	}
	if (raw.empty()) {
		err = name + " has no driving data in it. Teslas add it from software 2025.44.25 on (HW3 and newer), and not in Sentry clips while parked.";
		return false;
	}
	clip.start = startFromName(name);
	if (clip.start == 0) { err = name + " doesn't start with the date and time TeslaCam names its clips by (2026-10-31_14-03-10-front.mp4)."; return false; }
	// One message per frame at a steady frame rate: spread them evenly over the clip.
	double step = clip.duration > 0 ? clip.duration / raw.size() : 1.0 / 36;
	for (size_t i = 0; i < raw.size(); i++) raw[i].t = clip.start + i * step;
	clip.samples = std::move(raw);
	return true;
}

namespace {

// Mean, range and coverage of the car's speed in drive over [from, to].
PassSpeed speedBetween(const std::vector<Clip>& clips, double from, double to) {
	PassSpeed r;
	double sum = 0, covered0 = 1e300, covered1 = -1e300;
	r.minMph = 1e9;
	for (const Clip& c : clips)
		for (const Sample& s : c.samples) {
			if (s.t < from || s.t > to) continue;
			covered0 = std::min(covered0, s.t);
			covered1 = std::max(covered1, s.t);
			if (!s.drive) continue;
			double mph = s.mps * kMphPerMps;
			sum += mph;
			r.minMph = std::min(r.minMph, mph);
			r.maxMph = std::max(r.maxMph, mph);
			r.samples++;
		}
	// The clips must cover the window, not just touch it: most of it, at both ends.
	r.found = r.samples > 0 && covered0 <= from + 1 && covered1 >= to - 1;
	if (r.samples) r.mph = sum / r.samples;
	else r.minMph = 0;
	return r;
}

bool hasFix(const Sample& s) { return s.lat != 0 || s.lon != 0; }

}  // namespace

PassSpeed speedDuring(const std::vector<Clip>& clips, double passEnd, double crossing, double slack) {
	return speedBetween(clips, passEnd - crossing - slack, passEnd + 1 + slack);   // +1: the log time is truncated to the second
}

double distanceM(double lat1, double lon1, double lat2, double lon2) {
	const double kDeg = 3.14159265358979323846 / 180, kEarth = 6371000;
	double x = (lon2 - lon1) * kDeg * std::cos((lat1 + lat2) / 2 * kDeg), y = (lat2 - lat1) * kDeg;
	return std::sqrt(x * x + y * y) * kEarth;
}

bool positionAt(const std::vector<Clip>& clips, double t, double& lat, double& lon) {
	const Sample *before = nullptr, *after = nullptr;
	for (const Clip& c : clips)
		for (const Sample& s : c.samples) {
			if (!hasFix(s)) continue;
			if (s.t <= t && (!before || s.t > before->t)) before = &s;
			if (s.t >= t && (!after || s.t < after->t)) after = &s;
		}
	if (!before || !after || after->t - before->t > 2) return false;   // no fix within a second either side
	double f = after->t > before->t ? (t - before->t) / (after->t - before->t) : 0;
	lat = before->lat + f * (after->lat - before->lat);
	lon = before->lon + f * (after->lon - before->lon);
	return true;
}

std::vector<double> closestApproaches(const std::vector<Clip>& clips, double lat, double lon, double maxM) {
	// The samples with a fix, in time order across clips, each with its distance to the spot.  A drive-by is a run of
	// samples within maxM; its moment is the closest one.  GPS updates slower than the frame rate, so a run of equal
	// distances is common: take the middle of the closest run.
	struct D { double t, m; bool drive; };
	std::vector<D> ds;
	for (const Clip& c : clips)
		for (const Sample& s : c.samples)
			if (hasFix(s)) ds.push_back({ s.t, distanceM(lat, lon, s.lat, s.lon), s.drive });
	std::sort(ds.begin(), ds.end(), [](const D& a, const D& b) { return a.t < b.t; });
	std::vector<double> out;
	for (size_t i = 0; i < ds.size();) {
		if (ds[i].m > maxM || !ds[i].drive) { i++; continue; }
		size_t j = i;
		double best = ds[i].m, first = ds[i].t, last = ds[i].t;
		for (; j < ds.size() && ds[j].m <= maxM && ds[j].drive && ds[j].t - (j > i ? ds[j - 1].t : ds[j].t) < 2; j++) {
			if (ds[j].m < best - 1e-9) { best = ds[j].m; first = last = ds[j].t; }
			else if (ds[j].m <= best + 1e-9) last = ds[j].t;
		}
		out.push_back((first + last) / 2);
		i = j;
	}
	return out;
}

PassSpeed speedAtSpot(const std::vector<Clip>& clips, double lat, double lon, double passEnd, double crossing, double slack) {
	std::vector<double> near = closestApproaches(clips, lat, lon);
	if (near.empty()) return speedDuring(clips, passEnd, crossing);
	// VST's guess at the moment mid-zone: the log time is the second line, truncated to the second.
	double guess = passEnd + .5 - crossing / 2;
	double at = *std::min_element(near.begin(), near.end(), [&](double a, double b) { return std::fabs(a - guess) < std::fabs(b - guess); });
	PassSpeed r = speedBetween(clips, at - crossing / 2 - slack, at + crossing / 2 + slack);
	r.byGps = true;
	r.clockOffset = at - guess;
	return r;
}

bool readRide(const std::string& path, Clip& clip, std::string& err) {
	clip = Clip();
	clip.kind = Clip::Phone;
	clip.path = path;
	std::string name = std::filesystem::u8path(path).filename().u8string();
	std::ifstream in(std::filesystem::u8path(path));
	if (!in) { err = "Can't open " + name + "."; return false; }
	std::string line;
	if (!std::getline(in, line) || line.rfind("# VST ride-along recording", 0) != 0) {
		err = name + " isn't a VST Ride-along recording.";
		return false;
	}
	std::getline(in, line);   // column names
	struct Fix { double t, lat, lon, mps, acc; };
	std::vector<Fix> fixes;
	while (std::getline(in, line)) {
		// Empty fields (no speed, accuracy or heading) are allowed, so parse by hand rather than with %lf runs.
		double v[5] = { 0, 0, 0, -1, 0 };
		size_t pos = 0;
		for (int i = 0; i < 5 && pos <= line.size(); i++) {
			size_t comma = line.find(',', pos);
			std::string f = line.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
			if (!f.empty()) { try { v[i] = std::stod(f); } catch (...) { v[i] = i == 3 ? -1 : 0; } }
			if (comma == std::string::npos) break;
			pos = comma + 1;
		}
		if (v[0] <= 0 || (v[1] == 0 && v[2] == 0) || v[4] > kMaxAccuracyM) continue;
		fixes.push_back({ v[0] / 1000, v[1], v[2], v[3], v[4] });
	}
	if (fixes.size() < 3) { err = name + " has too few good GPS fixes to use."; return false; }
	std::sort(fixes.begin(), fixes.end(), [](const Fix& a, const Fix& b) { return a.t < b.t; });
	for (size_t i = 0; i < fixes.size(); i++) {
		Sample s;
		s.t = fixes[i].t;
		s.lat = fixes[i].lat;
		s.lon = fixes[i].lon;
		s.drive = true;
		s.mps = fixes[i].mps;
		if (s.mps < 0) {   // no GPS speed: distance over time between the neighbouring fixes
			const Fix& a = fixes[i ? i - 1 : 0];
			const Fix& b = fixes[std::min(i + 1, fixes.size() - 1)];
			s.mps = b.t > a.t ? distanceM(a.lat, a.lon, b.lat, b.lon) / (b.t - a.t) : 0;
		}
		clip.samples.push_back(s);
	}
	clip.start = clip.samples.front().t;
	clip.duration = clip.samples.back().t - clip.start;
	return true;
}

bool readAny(const std::string& path, Clip& clip, std::string& err) {
	std::string ext = std::filesystem::u8path(path).extension().u8string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
	return ext == ".csv" ? readRide(path, clip, err) : readClip(path, clip, err);
}

Joint jointSpeed(const std::vector<Clip>& clips, double lat, double lon, double passEnd, double crossing) {
	Joint j;
	auto read = [&](Clip::Kind k) {
		std::vector<Clip> some;
		for (const Clip& c : clips) if (c.kind == k) some.push_back(c);
		if (some.empty()) return PassSpeed();
		return lat != 0 || lon != 0 ? speedAtSpot(some, lat, lon, passEnd, crossing) : speedDuring(some, passEnd, crossing);
	};
	j.tesla = read(Clip::Tesla);
	j.phone = read(Clip::Phone);
	j.found = j.tesla.found || j.phone.found;
	j.both = j.tesla.steady() && j.phone.steady();
	j.agree = j.both && std::fabs(j.tesla.mph - j.phone.mph) <= Joint::kAgreeMph;
	if (j.agree) j.mph = (j.tesla.mph + j.phone.mph) / 2;
	else if (j.tesla.steady() || (j.tesla.found && !j.phone.steady())) j.mph = j.tesla.mph;
	else if (j.phone.found) j.mph = j.phone.mph;
	j.steady = j.tesla.steady() || j.phone.steady();
	return j;
}

}  // namespace dashcam
