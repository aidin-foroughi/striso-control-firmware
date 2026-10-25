// Standard MIDI File reader (smf.h).
#include "striso_sdk/smf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace striso::sdk {

namespace {

struct Reader {
	std::span<const uint8_t> d;
	size_t p = 0;
	bool bad = false;
	[[nodiscard]] bool has(size_t n) const { return p + n <= d.size(); }
	uint8_t u8() {
		if (!has(1)) {
			bad = true;
			return 0;
		}
		return d[p++];
	}
	uint32_t be(int n) {
		uint32_t v = 0;
		for (int i = 0; i < n; i++) v = (v << 8) | u8();
		return v;
	}
	uint32_t vlq() {
		uint32_t v = 0;
		for (int i = 0; i < 4; i++) {
			const uint8_t b = u8();
			v = (v << 7) | (b & 0x7fu);
			if (!(b & 0x80u)) return v;
		}
		bad = true;
		return v;
	}
};

struct Timed {
	uint64_t tick;
	int track;
	size_t seq;
	MidiEvent e;
	uint32_t tempo; // a tempo change (microseconds per quarter) if e.len == 0
};

} // namespace

MidiFile parse_midi(std::span<const uint8_t> data) {
	MidiFile f;
	Reader r{data};
	if (!r.has(14) || r.be(4) != 0x4d546864u) { // "MThd"
		f.error = "not a MIDI file (no MThd)";
		return f;
	}
	const uint32_t hlen = r.be(4);
	f.format = static_cast<int>(r.be(2));
	f.tracks = static_cast<int>(r.be(2));
	f.division = static_cast<int>(r.be(2));
	r.p += hlen > 6 ? hlen - 6 : 0;
	if (f.format > 1) {
		f.error = "MIDI file format " + std::to_string(f.format) + " (only 0 and 1)";
		return f;
	}
	if (f.division == 0) {
		f.error = "MIDI file division 0";
		return f;
	}
	std::vector<Timed> ev;
	for (int t = 0; t < f.tracks; t++) {
		if (!r.has(8)) {
			f.error = "MIDI file ends before track " + std::to_string(t + 1);
			return f;
		}
		const uint32_t id = r.be(4), len = r.be(4);
		const size_t end = r.p + len;
		if (end > data.size()) {
			f.error = "MIDI track " + std::to_string(t + 1) + " runs past the end of the file";
			return f;
		}
		if (id != 0x4d54726bu) { // not "MTrk": skip
			r.p = end;
			continue;
		}
		uint64_t tick = 0;
		uint8_t running = 0;
		while (r.p < end && !r.bad) {
			tick += r.vlq();
			uint8_t b = r.u8();
			if (b == 0xff) { // meta
				const uint8_t type = r.u8();
				const uint32_t n = r.vlq();
				if (type == 0x51 && n == 3) {
					const uint32_t us = r.be(3);
					ev.push_back({tick, t, ev.size(), {0, {0, 0, 0}, 0}, us});
				} else {
					r.p += n;
				}
				if (type == 0x2f) ev.push_back({tick, t, ev.size(), {0, {0, 0, 0}, 0}, 0}); // end of track
				continue;
			}
			if (b == 0xf0 || b == 0xf7) { // SysEx
				r.p += r.vlq();
				continue;
			}
			uint8_t d1;
			if (b & 0x80u) {
				running = b;
				d1 = r.u8();
			} else {
				if (!running) {
					f.error = "MIDI track " + std::to_string(t + 1) + ": data byte without a status";
					return f;
				}
				d1 = b;
				b = running;
			}
			const int type = b & 0xf0;
			MidiEvent e{0, {b, d1, 0}, 2};
			if (type != 0xc0 && type != 0xd0) {
				e.bytes[2] = r.u8();
				e.len = 3;
			}
			ev.push_back({tick, t, ev.size(), e, 0});
		}
		if (r.bad) {
			f.error = "MIDI track " + std::to_string(t + 1) + " is cut short";
			return f;
		}
		r.p = end;
	}
	std::stable_sort(ev.begin(), ev.end(), [](const Timed& a, const Timed& b) { return a.tick < b.tick; });
	// ticks to seconds: the tempo map (120 bpm until the first tempo), or SMPTE time
	const bool smpte = (f.division & 0x8000) != 0;
	const double smpte_tick = smpte ? 1.0 / (static_cast<double>(-static_cast<int8_t>(f.division >> 8)) * (f.division & 0xff)) : 0.0;
	double us_per_q = 500000.0, t0 = 0.0;
	uint64_t tick0 = 0;
	for (const Timed& x : ev) {
		const double t = smpte ? static_cast<double>(x.tick) * smpte_tick : t0 + static_cast<double>(x.tick - tick0) * us_per_q * 1e-6 / f.division;
		f.seconds = std::max(f.seconds, t);
		if (x.e.len == 0) {
			if (x.tempo) {
				t0 = t;
				tick0 = x.tick;
				us_per_q = x.tempo;
			}
			continue;
		}
		MidiEvent e = x.e;
		e.t = t;
		f.events.push_back(e);
	}
	return f;
}

MidiFile read_midi_file(const char* path) {
	MidiFile f;
	FILE* fp = std::fopen(path, "rb");
	if (!fp) {
		f.error = std::string("cannot open ") + path;
		return f;
	}
	std::vector<uint8_t> d;
	uint8_t buf[4096];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) d.insert(d.end(), buf, buf + n);
	std::fclose(fp);
	return parse_midi(d);
}

std::vector<uint8_t> make_midi(std::span<const MidiEvent> events) {
	constexpr int division = 480; // at 120 bpm: 960 ticks per second
	std::vector<uint8_t> trk;
	const auto vlq = [&trk](uint32_t v) {
		uint8_t b[5];
		int n = 0;
		b[n++] = v & 0x7fu;
		while (v >>= 7) b[n++] = 0x80u | (v & 0x7fu);
		while (n) trk.push_back(b[--n]);
	};
	uint64_t last = 0;
	for (const MidiEvent& e : events) {
		const uint64_t tick = static_cast<uint64_t>(std::llround(std::max(e.t, 0.0) * 960.0));
		vlq(static_cast<uint32_t>(tick > last ? tick - last : 0));
		last = std::max(last, tick);
		trk.insert(trk.end(), e.bytes, e.bytes + e.len);
	}
	vlq(0);
	trk.insert(trk.end(), {0xff, 0x2f, 0x00});
	std::vector<uint8_t> out = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, division >> 8, division & 0xff, 'M', 'T', 'r', 'k'};
	const uint32_t len = static_cast<uint32_t>(trk.size());
	for (int i = 3; i >= 0; i--) out.push_back(static_cast<uint8_t>(len >> (8 * i)));
	out.insert(out.end(), trk.begin(), trk.end());
	return out;
}

} // namespace striso::sdk
