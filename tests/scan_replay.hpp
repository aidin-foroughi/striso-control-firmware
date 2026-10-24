// Replaying a raw scan recording (StcR, key_scanner_capture.hpp) through the KeyScanner on the PC,
// and comparing it with what the board sent: per scan, the number of messages and their hash as
// the board recorded them, and the messages themselves with the StcS stream (14-bit values, as
// ThreadSend sends them).
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "key_scanner_capture.hpp"

namespace scanreplay {

struct Msg {
	std::array<int, 6> v{};
	bool queued = true; // false: msgSend refused it (the queue was full)
	uint32_t frame = 0; // index of the scan in the recording
};

struct Result {
	std::string error; // empty: the recording decoded
	ks::CaptureHeader head;
	uint32_t frames = 0;
	uint32_t mismatched = 0; // scans whose message count or hash differs from the board's
	uint32_t first_mismatch = 0;
	uint32_t torn = 0; // scans the board marked torn
	uint32_t refused = 0;
	std::vector<Msg> msgs;
	std::vector<uint32_t> cycles; // per scan, DWT cycles since the previous one
	bool has_stats = false;
	ks::NoiseStats stats;
};

// the board's sink as the recording saw it: the refused attempts are refused again
struct ReplaySink {
	const ks::FrameRecord* rec = nullptr;
	uint32_t attempt = 0;
	uint32_t hash = ks::kFnvBasis;
	std::vector<Msg>* out = nullptr;
	uint32_t frame = 0;
	int send(const int (&msg)[6]) {
		bool fail = false;
		for (int i = 0; i < rec->nfails; i++) fail = fail || rec->fails[i] == attempt;
		attempt++;
		hash = ks::fnv1a(hash, msg);
		Msg m;
		std::copy(msg, msg + 6, m.v.begin());
		m.queued = !fail;
		m.frame = frame;
		out->push_back(m);
		return fail ? 1 : 0;
	}
	void wait() {}
};

inline Result replay(const std::vector<uint8_t>& blob) {
	Result res;
	ks::KeyScanner scanner;
	if (!ks::read_header(blob.data(), blob.size(), res.head, scanner)) {
		res.error = "not a KSR1 recording";
		return res;
	}
	const size_t start = ks::kHeaderBytes;
	if (blob.size() < start + res.head.record_bytes) {
		res.error = "recording cut short: " + std::to_string(blob.size() - start) + " of " + std::to_string(res.head.record_bytes) + " record bytes";
		return res;
	}
	ks::RecordReader reader(ks::ByteReader{blob.data() + start, blob.data() + start + res.head.record_bytes});
	reader.reset(res.head.first_seq, res.head.first_t_start);
	ks::FrameRecord rec;
	while (reader.next(rec)) {
		if (rec.has_config) scanner.cfg = rec.cfg;
		if (rec.has_cal) {
			for (int b = 0; b < ks::kButtons; b++) scanner.keys[b].c_force = rec.c_force[b];
		}
		ReplaySink sink;
		sink.rec = &rec;
		sink.out = &res.msgs;
		sink.frame = res.frames;
		scanner.process(rec.frame, sink);
		const bool same = sink.attempt == rec.attempts && (rec.attempts == 0 || (sink.hash & 0xffffU) == rec.hash);
		if (!same && res.mismatched++ == 0) res.first_mismatch = res.frames;
		if (rec.end_flags & ks::kEndTorn) res.torn++;
		res.cycles.push_back(rec.cycles);
		res.refused += static_cast<uint32_t>(rec.nfails);
		res.frames++;
	}
	if (res.head.stats_bytes == static_cast<uint32_t>(ks::kStatsBytes) && blob.size() >= start + res.head.record_bytes + res.head.stats_bytes) {
		ks::ByteReader sr{blob.data() + start + res.head.record_bytes, blob.data() + blob.size()};
		ks::read_stats(sr, res.stats);
		res.has_stats = sr.ok;
	}
	if (!reader.ok()) res.error = "malformed record at scan " + std::to_string(res.frames);
	else if (res.frames != res.head.frames) res.error = "recording has " + std::to_string(res.frames) + " scans, header says " + std::to_string(res.head.frames);
	return res;
}

// a key message as the StcS stream carries it: values saturated to signed 14 bit (ThreadSend)
[[nodiscard]] inline std::array<int, 6> on_wire(const std::array<int, 6>& v) {
	std::array<int, 6> w = v;
	for (int c = 2; c < 6; c++) w[c] = std::clamp(w[c], -8192, 8191);
	return w;
}

// the queued messages must be a contiguous run of the stream's key messages (the stream starts
// before the recording and ends after it). Returns the offset, -1 if they aren't, and how many
// places they fit (more than one only for very short or empty runs).
inline long find_in_stream(const std::vector<Msg>& msgs, const std::vector<std::array<int, 6>>& stream, int* matches) {
	std::vector<std::array<int, 6>> want;
	for (const Msg& m : msgs) {
		if (m.queued) want.push_back(on_wire(m.v));
	}
	long first = -1;
	*matches = 0;
	if (want.size() > stream.size()) return -1;
	for (size_t off = 0; off + want.size() <= stream.size(); off++) {
		if (std::equal(want.begin(), want.end(), stream.begin() + static_cast<long>(off))) {
			if (first < 0) first = static_cast<long>(off);
			++*matches;
		}
	}
	return first;
}

// the key messages of a stream CSV (tools/record_raw.py, tools/record_keys.py: t,src,id,pres,velo,x,y)
inline std::vector<std::array<int, 6>> read_stream_csv(const char* path) {
	std::vector<std::array<int, 6>> out;
	FILE* f = std::fopen(path, "r");
	if (!f) return out;
	char line[256];
	if (!std::fgets(line, sizeof line, f)) {
		std::fclose(f);
		return out;
	}
	while (std::fgets(line, sizeof line, f)) {
		double t = 0;
		std::array<int, 6> v{};
		if (std::sscanf(line, "%lf,%d,%d,%d,%d,%d,%d", &t, &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 7 && v[0] == 0) out.push_back(v);
	}
	std::fclose(f);
	return out;
}

} // namespace scanreplay
