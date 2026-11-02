#include "Sha256.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace sha256 {

namespace {
const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// The running hash: blocks of 64 bytes go through block(); finish() pads with 0x80, zeros and the bit length.
struct State {
	uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	unsigned char buf[64];
	size_t used = 0;
	uint64_t bits = 0;
	void block(const unsigned char* p) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
			uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
	}
	void add(const unsigned char* p, size_t n) {
		bits += uint64_t(n) * 8;
		while (n) {
			size_t take = std::min(n, sizeof buf - used);
			for (size_t i = 0; i < take; i++) buf[used + i] = p[i];
			used += take; p += take; n -= take;
			if (used == sizeof buf) { block(buf); used = 0; }
		}
	}
	std::string finish() {
		uint64_t total = bits;
		unsigned char pad = 0x80;
		add(&pad, 1);
		unsigned char zero = 0;
		while (used != 56) add(&zero, 1);
		unsigned char len[8];
		for (int i = 0; i < 8; i++) len[i] = (unsigned char)(total >> (56 - 8 * i));
		add(len, 8);
		char out[65];
		for (int i = 0; i < 8; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
		return std::string(out, 64);
	}
};
}  // namespace

std::string hex(const std::string& bytes) {
	State s;
	s.add(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
	return s.finish();
}

std::string hexOfFile(const std::string& path) {
	std::ifstream in(std::filesystem::path(path), std::ios::binary);
	if (!in) return "";
	State s;
	std::vector<char> chunk(1 << 16);
	while (in) {
		in.read(chunk.data(), std::streamsize(chunk.size()));
		std::streamsize got = in.gcount();
		if (got > 0) s.add(reinterpret_cast<const unsigned char*>(chunk.data()), size_t(got));
	}
	return s.finish();
}

}  // namespace sha256
