// The virtual board's API: boot, the console, keys and sound, settings keys and LEDs, MIDI out,
// resets, the bootloader drive and its config, determinism.
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "board.h"
#include "doctest/doctest.h"

using vt::Board;
using vt::lines;
namespace fs = std::filesystem;

namespace {

constexpr int SR = 48000;
constexpr int IDC_ALT = 3; // striso.h

double rms(const std::vector<float>& x) {
	double s = 0;
	for (float v : x) s += static_cast<double>(v) * v;
	return x.empty() ? 0 : std::sqrt(s / static_cast<double>(x.size()));
}

bool finite(const std::vector<float>& x) {
	for (float v : x)
		if (!std::isfinite(v)) return false;
	return true;
}

std::vector<float> render(Board& bd, double seconds) {
	std::vector<float> out(static_cast<size_t>(seconds * SR) * 2);
	REQUIRE(sv_render(bd.b, out.data(), static_cast<int32_t>(out.size() / 2)) == static_cast<int32_t>(out.size() / 2));
	return out;
}

int voice_entry(Board& bd, const std::string& name) {
	for (const std::string& l : lines(bd.cmd("v", 40)))
		if (l.size() > 4 && l.substr(l.find(' ', 2) + 1) == name) return std::atoi(l.c_str() + 2);
	return -1;
}

sv_state state(Board& bd) {
	sv_state st{};
	st.size = sizeof st;
	sv_get_state(bd.b, &st);
	return st;
}

// a config UF2 (tools/striso.py config_uf2): records between "MConfig v1.0" and "MCfgEnd"
std::vector<uint8_t> config_uf2(const std::vector<std::pair<std::string, std::string>>& recs) {
	std::string data;
	auto field = [&](std::string s) {
		s.resize(8, ' ');
		data += s;
	};
	field("MConfig ");
	field("v1.0");
	for (const auto& [k, v] : recs) {
		field(k);
		field(v);
	}
	field("MCfgEnd ");
	field("");
	data.resize((data.size() + 255) / 256 * 256, '\xff');
	const uint32_t n = static_cast<uint32_t>(data.size() / 256);
	std::vector<uint8_t> out(n * 512, 0);
	auto w32 = [&](size_t o, uint32_t v) {
		for (int k = 0; k < 4; k++) out[o + k] = static_cast<uint8_t>(v >> (8 * k));
	};
	for (uint32_t i = 0; i < n; i++) {
		const size_t o = i * 512;
		w32(o, 0x0A324655);
		w32(o + 4, 0x9E5D5157);
		w32(o + 8, 0x2000);
		w32(o + 12, SV_FLASH_ADDR + 256 * i);
		w32(o + 16, 256);
		w32(o + 20, i);
		w32(o + 24, n);
		w32(o + 28, 0xA21E1295);
		std::memcpy(&out[o + 32], data.data() + 256 * i, 256);
		w32(o + 508, 0x0AB16F30);
	}
	return out;
}

} // namespace

TEST_CASE("api: boots, attaches USB, answers StcV with the firmware version") {
	CHECK(sv_api_version() == SV_API_VERSION);
	Board bd;
	REQUIRE(bd.b != nullptr);
	CHECK(sv_usb_state(bd.b) == SV_USB_ATTACHED);
	CHECK(sv_usb_generation(bd.b) == 1u);
	const std::string v = bd.cmd("V");
	CHECK(v.rfind(std::string(sv_firmware_version()) + " ", 0) == 0);
	CHECK(v.find("virtual         Striso board\r\nCalibration date: virtual\r\n") != std::string::npos);
	const sv_state st = state(bd);
	CHECK(st.voices == 65);
	CHECK(st.voice == 1); // va:classic at power on
	CHECK(st.blocks > 0);
}

TEST_CASE("api: the USB console is not there while the board boots") {
	sv_options o{};
	o.size = sizeof o;
	o.boot_ms = 100;
	Board bd(&o);
	CHECK(sv_usb_state(bd.b) == SV_USB_DETACHED);
	uint8_t c = 'S';
	CHECK(sv_console_write(bd.b, &c, 1) == SV_ENODEV);
	bd.run_ms(500); // InitPConnection waits 500 ms before it connects
	CHECK(sv_usb_state(bd.b) == SV_USB_ATTACHED);
}

TEST_CASE("api: a key sounds on the selected voice, finite, and falls silent after its release") {
	Board bd;
	for (const char* voice : {"va:classic", "bowed:violin", "ep:stage", "brass:trumpet"}) {
		CAPTURE(voice);
		const int n = voice_entry(bd, voice);
		REQUIRE(n >= 0);
		REQUIRE(sv_select_voice(bd.b, n) == 0);
		render(bd, 0.1);
		const std::vector<float> before = render(bd, 0.2);
		sv_inject_key(bd.b, 0, 30, 4000, 3000, 0, 0); // D_3
		std::vector<float> on;
		for (int i = 0; i < 40; i++) { // pressure held, the scanner sends ~every ms
			sv_inject_key(bd.b, 0, 30, 4000 + 50 * i, 100, 0, 0);
			const std::vector<float> x = render(bd, 0.01);
			on.insert(on.end(), x.begin(), x.end());
		}
		sv_inject_key(bd.b, 0, 30, 0, -2000, 0, 0);
		render(bd, 3.0);
		const std::vector<float> after = render(bd, 0.5);
		CHECK(finite(on));
		CHECK(rms(before) < 1e-4); // the effects' tail of the voice before
		CHECK(rms(on) > 1e-3);
		CHECK(rms(after) < 1e-4);
		MESSAGE(voice << ": rms while held " << rms(on));
	}
}

TEST_CASE("api: a live parameter change (Q) changes the render; Q to the same value does not") {
	auto take = [](float gain, bool set) {
		Board bd;
		REQUIRE(sv_select_voice(bd.b, 1) == 0); // va:classic
		render(bd, 0.05);
		if (set) {
			uint32_t u;
			std::memcpy(&u, &gain, 4);
			std::string q = "StcQ";
			q += '\0';
			q += '\0'; // gain
			for (int k = 0; k < 4; k++) q += static_cast<char>(u >> (8 * k));
			bd.send(q, 10);
		} else {
			bd.run_ms(10);
		}
		sv_inject_key(bd.b, 0, 30, 5000, 2000, 0, 0);
		return render(bd, 0.5);
	};
	const std::vector<float> plain = take(0, false), same = take(2.3f, true), louder = take(4.0f, true);
	CHECK(plain == same);
	CHECK(plain != louder);
	CHECK(rms(louder) > 1.5 * rms(plain));
}

TEST_CASE("api: the binary stream carries injected keys; the board's MIDI out sends MPE") {
	Board bd;
	bd.cmd("S", 10);
	sv_inject_key(bd.b, 0, 30, 3000, 2000, 0, 0);
	bd.run_ms(5);
	const std::string s = bd.read();
	CHECK(s.find(std::string("\x84\x1e", 2)) != std::string::npos); // src 0, 4 values, id 30
	uint8_t midi[256];
	const int n = sv_midi_read(bd.b, midi, sizeof midi);
	bool note_on = false;
	for (int i = 0; i + 3 < n; i += 4) note_on = note_on || ((midi[i + 1] & 0xf0) == 0x90 && midi[i + 3] > 0);
	CHECK(note_on);
}

TEST_CASE("api: settings + a voice key switch the voice and show its colour; the LEDs") {
	Board bd;
	sv_leds l{};
	sv_get_leds(bd.b, &l);
	MESSAGE("at boot: rgb " << l.rgb[0] << " " << l.rgb[1] << " " << l.rgb[2]);
	sv_inject_control(bd.b, IDC_ALT, 1); // hold Settings
	bd.run_ms(10);
	sv_inject_key(bd.b, 0, 24, 3000, 2000, 0, 0); // the bowed key
	bd.run_ms(10);
	sv_get_leds(bd.b, &l);
	CHECK(state(bd).voice == voice_entry(bd, "bowed:violin"));
	CHECK(l.rgb[0] == 64);
	CHECK(l.rgb[1] == 0);
	CHECK(l.rgb[2] == 0);
	CHECK(l.updown[0] + l.updown[1] + l.updown[2] + l.updown[3] > 0); // the preset dial
	sv_inject_key(bd.b, 0, 24, 0, 0, 0, 0);
	sv_inject_control(bd.b, IDC_ALT, 0);
	bd.run_ms(10);
	sv_get_leds(bd.b, &l);
	CHECK(l.rgb[1] == 0xaa); // back to the tuning's colour (12-TET green)
}

TEST_CASE("api: settings + key 67 resets the board when Settings is let go; it comes back") {
	Board bd;
	sv_inject_control(bd.b, IDC_ALT, 1);
	sv_inject_key(bd.b, 0, 67, 3000, 0, 0, 0);
	sv_inject_key(bd.b, 0, 67, 0, 0, 0, 0);
	bd.run_ms(50);
	CHECK(state(bd).resets == 0); // waits for the release of Settings
	sv_inject_control(bd.b, IDC_ALT, 0);
	bd.run_ms(300);
	CHECK(state(bd).resets == 1);
	CHECK(sv_usb_state(bd.b) == SV_USB_DETACHED);
	bd.run_ms(1000);
	CHECK(sv_usb_state(bd.b) == SV_USB_ATTACHED);
	CHECK(sv_usb_generation(bd.b) == 2u);
	CHECK(bd.cmd("V").rfind(sv_firmware_version(), 0) == 0);
}

TEST_CASE("api: StcB: the bootloader's drive; a config written there is flashed and kept") {
	Board bd;
	bd.cmd("B", 50);
	REQUIRE(sv_usb_state(bd.b) == SV_USB_BOOTLOADER);
	char path[1024];
	REQUIRE(sv_drive_path(bd.b, path, sizeof path) > 0);
	const fs::path drive(path);
	CHECK(fs::exists(drive / "INFO_UF2.TXT"));
	CHECK(fs::exists(drive / "CONFIG.UF2"));
	{
		std::ifstream f(drive / "INFO_UF2.TXT");
		std::string info((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		CHECK(info.find("Striso") != std::string::npos);
	}
	const std::vector<uint8_t> uf2 = config_uf2({{"iGoct", "1"}, {"fCfacD_3", "1.5"}, {"fP1presS", "2.0"}, {"iGpreset", "1"}});
	{
		std::ofstream f(drive / "NEWCFG.UF2", std::ios::binary);
		f.write(reinterpret_cast<const char*>(uf2.data()), static_cast<std::streamsize>(uf2.size()));
	}
	bd.run_ms(300); // the drive is polled every 50 ms; then the board restarts
	CHECK(!fs::exists(drive / "INFO_UF2.TXT"));
	bd.run_ms(1000);
	REQUIRE(sv_usb_state(bd.b) == SV_USB_ATTACHED);
	const std::string k = bd.cmd("k", 40);
	CHECK(k.find("30 D_3 keys 3DCCCCCD 41200000 3FC00000 0") != std::string::npos); // 1.5
	CHECK(bd.cmd("g").find("0 pressure touch 3DCCCCCD 40800000 40000000 0") != std::string::npos); // 2.0
	CHECK(bd.cmd("N", 40).find(" 42940000 61\r\n") != std::string::npos); // base 74: an octave up

	// the flash survives the board
	std::vector<uint8_t> flash(SV_FLASH_SIZE);
	REQUIRE(sv_get_flash(bd.b, flash.data(), SV_FLASH_SIZE) == SV_FLASH_SIZE);
	sv_options o{};
	o.size = sizeof o;
	o.flash = flash.data();
	o.flash_len = SV_FLASH_SIZE;
	Board again(&o);
	CHECK(sv_usb_state(again.b) == SV_USB_ATTACHED);
	CHECK(again.cmd("k", 40) == k);
}

TEST_CASE("api: two boards are independent and deterministic") {
	Board a, b;
	sv_select_voice(a.b, 15);
	sv_select_voice(b.b, 15);
	sv_inject_key(a.b, 0, 30, 4000, 2000, 500, 0);
	sv_inject_key(b.b, 0, 30, 4000, 2000, 500, 0);
	const std::vector<float> x = render(a, 0.5), y = render(b, 0.5);
	CHECK(x == y);
	CHECK(rms(x) > 1e-3);
	sv_inject_key(a.b, 0, 30, 0, 0, 0, 0);
	a.cmd("Q\x02\x00\x00\x00\x00\x40", 5); // touch pressure 2 on a only
	CHECK(a.cmd("g") != b.cmd("g"));
}

TEST_CASE("api: real time: the board runs by itself, its audio comes through the FIFO") {
	Board bd;
	REQUIRE(sv_select_voice(bd.b, 1) == 0);
	REQUIRE(sv_realtime_start(bd.b, 0) == 0);
	CHECK(sv_render(bd.b, nullptr, 32) == SV_EBUSY);
	sv_inject_key(bd.b, 0, 30, 5000, 2000, 0, 0);
	std::vector<float> got;
	for (int i = 0; i < 60 && got.size() < 9600; i++) {
		float buf[2 * 4800];
		const int n = sv_read_audio(bd.b, buf, 4800);
		got.insert(got.end(), buf, buf + 2 * n);
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	sv_realtime_stop(bd.b);
	CHECK(got.size() >= 9600);
	CHECK(rms(got) > 1e-4);
	const uint64_t t = sv_time_frames(bd.b);
	CHECK(t > static_cast<uint64_t>(SR)); // boot + real time
}

TEST_CASE("api: sv_audio_source renders for an output's callback, at 48 kHz exactly, resampled at others") {
	Board a, b;
	sv_select_voice(a.b, 1);
	sv_select_voice(b.b, 1);
	sv_inject_key(a.b, 0, 30, 5000, 2000, 0, 0);
	sv_inject_key(b.b, 0, 30, 5000, 2000, 0, 0);
	std::vector<float> x(2 * 4800), y(2 * 4800);
	for (int k = 0; k < 10; k++) CHECK(sv_audio_source(a.b, x.data() + 2 * 480 * k, 480, 48000) == 480);
	REQUIRE(sv_render(b.b, y.data(), 4800) == 4800);
	CHECK(x == y); // the same as sv_render
	std::vector<float> z(2 * 4410);
	for (int k = 0; k < 10; k++) CHECK(sv_audio_source(a.b, z.data() + 2 * 441 * k, 441, 44100) == 441);
	CHECK(finite(z));
	CHECK(rms(z) > 1e-3);
	CHECK(sv_time_frames(a.b) - sv_time_frames(b.b) >= 4799); // 0.1 s more at 44.1 kHz
	REQUIRE(sv_realtime_start(a.b, 0) == 0);
	CHECK(sv_audio_source(a.b, z.data(), 441, 44100) == 0); // one of the two drives the board
	sv_realtime_stop(a.b);
}
