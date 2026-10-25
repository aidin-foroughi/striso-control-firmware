// USB MIDI 2.0 on the virtual board: interface 1's alternate setting 1 (usbcfg.c), its Group
// Terminal Block, and what goes out and comes in on it per MIDI mode. MPE and normal are what they
// were on alt 0, and the same bytes as MIDI 1.0 in UMP on alt 1; mode midi2 is MPE on alt 0 and
// the keys as standard MIDI 2.0 (ump_out.h) on alt 1. USB MIDI 2.0 in plays as sv_ump_write does.
#include <cstdint>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

using vt::Board;

namespace {

// a user config: the board boots preset 1, in MIDI mode `mode`
std::string config(const std::string& mode) {
	std::string data;
	auto field = [&](std::string s) {
		s.resize(8, ' ');
		data += s;
	};
	field("MConfig ");
	field("v1.0");
	field("iGpreset");
	field("1");
	field("sP1Mmode");
	field(mode);
	field("MCfgEnd ");
	field("");
	return data;
}

struct MidiBoard {
	std::string flash;
	sv_options o{};
	Board bd;
	explicit MidiBoard(const std::string& mode) : flash(config(mode)), o(opts(flash)), bd(&o) {
		REQUIRE(sv_select_voice(bd.b, 0) == 0);
		bd.run_ms(300);
		read();
	}
	static sv_options opts(const std::string& f) {
		sv_options x{};
		x.size = sizeof x;
		x.flash = reinterpret_cast<const uint8_t*>(f.data());
		x.flash_len = static_cast<uint32_t>(f.size());
		return x;
	}
	int32_t set_alt(int alt) { return sv_usb_control(bd.b, 0x01, 0x0b, static_cast<uint16_t>(alt), 1, nullptr, 0); }
	// everything the board sent on the MIDI IN endpoint since the last read
	std::vector<uint8_t> read() {
		std::vector<uint8_t> out;
		uint8_t buf[4096];
		int32_t n;
		while ((n = sv_midi_read(bd.b, buf, sizeof buf)) > 0) out.insert(out.end(), buf, buf + n);
		return out;
	}
	// a key played: pressed, moved, released
	std::vector<uint8_t> play(int key) {
		sv_inject_key(bd.b, 0, key, 3000, 2500, 400, -300);
		bd.run_ms(20);
		sv_inject_key(bd.b, 0, key, 6000, 200, 900, 1200);
		bd.run_ms(20);
		sv_inject_key(bd.b, 0, key, 0, -1500, 0, 0);
		bd.run_ms(50);
		return read();
	}
};

std::vector<uint32_t> words(const std::vector<uint8_t>& b) {
	std::vector<uint32_t> w;
	for (size_t i = 0; i + 3 < b.size(); i += 4)
		w.push_back(static_cast<uint32_t>(b[i]) | static_cast<uint32_t>(b[i + 1]) << 8 | static_cast<uint32_t>(b[i + 2]) << 16 |
		            static_cast<uint32_t>(b[i + 3]) << 24);
	return w;
}

} // namespace

TEST_CASE("usb midi2: interface 1 has alt 1 with its Group Terminal Block; SET/GET_INTERFACE") {
	MidiBoard m("mpe");
	uint8_t alt = 0xff;
	CHECK(sv_usb_control(m.bd.b, 0x81, 0x0a, 0, 1, &alt, 1) == 1);
	CHECK(alt == 0);
	CHECK(m.set_alt(1) == 0);
	CHECK(sv_usb_control(m.bd.b, 0x81, 0x0a, 0, 1, &alt, 1) == 1);
	CHECK(alt == 1);
	CHECK(m.set_alt(2) == SV_ESTATE); // no alt 2
	uint8_t gtb[64] = {};
	const int32_t n = sv_usb_control(m.bd.b, 0x81, 0x06, 0x2601, 1, gtb, sizeof gtb);
	REQUIRE(n == 18);
	const std::vector<uint8_t> want = {0x05, 0x26, 0x01, 0x12, 0x00, 0x0d, 0x26, 0x02, 0x01, 0x00, 0x00, 0x01, 0x02, 0x11, 0, 0, 0, 0};
	CHECK(std::vector<uint8_t>(gtb, gtb + n) == want);
	CHECK(m.set_alt(0) == 0);
}

TEST_CASE("usb midi2: MPE and normal on alt 1 are alt 0's bytes, as MIDI 1.0 in UMP") {
	for (const char* mode : {"mpe", "normal"}) {
		INFO(mode);
		MidiBoard a(mode), b(mode);
		REQUIRE(b.set_alt(1) == 0);
		const std::vector<uint8_t> p = a.play(30);
		const std::vector<uint32_t> w = words(b.play(30));
		REQUIRE(!p.empty());
		REQUIRE(w.size() * 4 == p.size());
		for (size_t i = 0; i < w.size(); i++) {
			const uint8_t* e = &p[4 * i];
			const uint32_t want = static_cast<uint32_t>((e[1] >= 0xf0 ? 1 : 2)) << 28 | static_cast<uint32_t>(e[0] >> 4) << 24 |
			                      static_cast<uint32_t>(e[1]) << 16 | static_cast<uint32_t>(e[2]) << 8 | e[3];
			CHECK(w[i] == want);
		}
	}
}

TEST_CASE("usb midi2: mode midi2 is MPE on alt 0, byte for byte, and MIDI 2.0 on alt 1") {
	MidiBoard mpe("mpe"), m0("midi2"), m1("midi2");
	const std::vector<uint8_t> want = mpe.play(30);
	CHECK(m0.play(30) == want); // alt 0: a MIDI 1.0 host gets MPE

	REQUIRE(m1.set_alt(1) == 0);
	const std::vector<uint32_t> w = words(m1.play(30));
	REQUIRE(w.size() >= 4);
	// no MIDI 1.0 note data (type 2 notes, pressure, bends) on alt 1
	static constexpr int ump_words[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
	for (size_t i = 0; i < w.size(); i += static_cast<size_t>(ump_words[w[i] >> 28])) {
		const uint32_t s = (w[i] >> 20) & 0xf;
		const bool midi1_note = (w[i] >> 28) == 2 && (s == 0x8 || s == 0x9 || s == 0xa || s == 0xd || s == 0xe);
		INFO(std::hex << w[i]);
		CHECK_FALSE(midi1_note);
	}
	// the note: Note On (velocity, Pitch 7.9), Pitch 7.25, then its controllers, ..., Note Off
	std::vector<std::pair<uint32_t, uint32_t>> m;
	for (size_t i = 0; i < w.size();) {
		if ((w[i] >> 28) == 4 && i + 1 < w.size()) {
			m.emplace_back(w[i], w[i + 1]);
			i += 2;
		} else {
			i += 1;
		}
	}
	REQUIRE(m.size() >= 3);
	CHECK((m[0].first & 0xfff000ffu) == 0x40900003u); // Note On, group 1 channel 1, attribute Pitch 7.9
	const uint32_t number = (m[0].first >> 8) & 0x7f;
	CHECK((m[0].second >> 16) > 0); // its velocity
	CHECK(m[1].first == (0x40000003u | number << 8)); // Pitch 7.25
	CHECK((m[1].second >> 16) == (m[0].second & 0xffff)); // (the Note On's Pitch 7.9 is its top)
	CHECK((m.back().first & 0xfff0ff00u) == (0x40800000u | number << 8)); // Note Off
	bool pressure = false;
	for (const auto& [h, v] : m) pressure |= (h & 0xfff0ff00u) == (0x40a00000u | number << 8) && v > 0;
	CHECK(pressure);
}

TEST_CASE("usb midi2: MIDI 2.0 in on alt 1 plays as sv_ump_write does") {
	MidiBoard a("mpe"), b("mpe");
	REQUIRE(b.set_alt(1) == 0);
	const uint32_t msgs[] = {0x40913c00u, 0xc0000000u, 0x40d10000u, 0x9abcdef0u}; // Note On ch 2, channel pressure
	REQUIRE(sv_ump_write(a.bd.b, msgs, 4) == 4);
	std::vector<uint8_t> bytes;
	for (const uint32_t x : msgs)
		for (int k = 0; k < 4; k++) bytes.push_back(static_cast<uint8_t>(x >> (8 * k)));
	REQUIRE(sv_midi_write(b.bd.b, bytes.data(), static_cast<int32_t>(bytes.size())) == static_cast<int32_t>(bytes.size()));
	a.bd.run_ms(100);
	b.bd.run_ms(100);
	sv_state sa{}, sb{};
	sa.size = sb.size = sizeof sa;
	REQUIRE(sv_get_state(a.bd.b, &sa) == 0);
	REQUIRE(sv_get_state(b.bd.b, &sb) == 0);
	bool sounding = false;
	for (size_t v = 0; v < std::size(sa.controls); v++) {
		for (int c = 0; c < 5; c++) CHECK(sa.controls[v][c] == sb.controls[v][c]);
		sounding |= sb.controls[v][1] == static_cast<float>(0x9abcdef0u / 4294967295.0);
	}
	CHECK(sounding);
}
