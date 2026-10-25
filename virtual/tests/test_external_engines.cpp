// Engines loaded from their component libraries (sv_options.engine_dir: the engines repository's
// components) on the virtual board, next to the compiled-in ones: they are listed after them, and
// the accordion and the trumpet loaded render bit for bit as compiled in, through the board's
// synth thread (its voice controls, its track_key), over every preset and the scripted
// performances, and the key takes.
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "awva_sdk/performance.h"
#include "striso_virtual.h"

namespace {

namespace sdk = awva::sdk;
namespace dsp = awva;
constexpr int SR = 48000, NV = 10, BLK = 32;

std::string console(sv_board* b, const std::string& cmd) {
	uint8_t buf[8192];
	while (sv_console_read(b, buf, sizeof buf) > 0) {
	}
	const std::string bytes = "Stc" + cmd;
	sv_console_write(b, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size()));
	sv_advance_ms(b, 40);
	std::string out;
	int32_t n;
	while ((n = sv_console_read(b, buf, sizeof buf)) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
	return out;
}

// the voice entries of an engine (Stcv), in the board's order
std::vector<int> entries_of(sv_board* b, const std::string& engine) {
	const std::string list = console(b, "v");
	std::vector<int> e;
	size_t p = 0;
	while (p < list.size()) {
		size_t end = list.find("\r\n", p);
		if (end == std::string::npos) end = list.size();
		const std::string line = list.substr(p, end - p);
		p = end + 2;
		// "<* or space> <n> <engine>[:<preset>]"
		if (line.size() < 4) continue;
		const size_t sp = line.find(' ', 2);
		if (sp == std::string::npos) continue;
		const std::string name = line.substr(sp + 1);
		if (name.substr(0, name.find(':')) == engine) e.push_back(std::atoi(line.c_str() + 2));
	}
	return e;
}

struct Board {
	sv_board* b;
	explicit Board(const char* dir) {
		sv_options o{};
		o.size = sizeof o;
		o.engine_dir = dir;
		b = sv_create(&o);
		REQUIRE(b != nullptr);
		sv_test(b, SV_TEST_MOTION_OFF, 0);
	}
	~Board() { sv_destroy(b); }
	void to_block() {
		sv_state st{};
		st.size = sizeof st;
		sv_get_state(b, &st);
		if (st.to_block > 0) sv_render_taps(b, st.to_block, nullptr, nullptr, nullptr);
	}
};

void render_pair(Board& a, int ea, Board& x, int ex, sdk::Player& player, double seconds, const std::string& what) {
	a.to_block();
	x.to_block();
	sv_select_voice(a.b, ea);
	sv_select_voice(x.b, ex);
	std::vector<dsp::VoiceControls> vc(NV, dsp::voice_at_rest);
	dsp::GlobalControls g = dsp::globals_at_rest;
	const int blocks = static_cast<int>(seconds * SR / BLK);
	std::vector<float> oa(static_cast<size_t>(blocks) * BLK), ox(oa.size());
	for (int k = 0; k < blocks; k++) {
		player.block(k, vc.data(), g);
		for (int v = 0; v < NV; v++) {
			const float c[5] = {vc[v].note, vc[v].pres, vc[v].vpres, vc[v].but_x, vc[v].but_y};
			sv_set_engine_controls(a.b, v, c);
			sv_set_engine_controls(x.b, v, c);
		}
		const float gv[8] = {g.acc_abs, g.acc_x, g.acc_y, g.acc_z, g.rot_x, g.rot_y, g.rot_z, g.pedal};
		sv_set_engine_controls(a.b, -1, gv);
		sv_set_engine_controls(x.b, -1, gv);
		REQUIRE(sv_render_taps(a.b, BLK, nullptr, &oa[static_cast<size_t>(k) * BLK], nullptr) == BLK);
		REQUIRE(sv_render_taps(x.b, BLK, nullptr, &ox[static_cast<size_t>(k) * BLK], nullptr) == BLK);
	}
	INFO(what);
	CHECK(std::memcmp(oa.data(), ox.data(), oa.size() * sizeof(float)) == 0);
	double e2 = 0;
	for (float s : oa) e2 += static_cast<double>(s) * s;
	CHECK(e2 > 0.0);
}

} // namespace

TEST_CASE("external engines: the components in engine_dir are listed after the compiled-in engines") {
	Board none(nullptr), with(NSC_COMPONENT_DIR);
	const std::vector<int> a = entries_of(none.b, "accordion"), b = entries_of(with.b, "accordion");
	REQUIRE(!a.empty());
	REQUIRE(b.size() == 2 * a.size()); // compiled in, then loaded
	for (size_t i = 0; i < a.size(); i++) CHECK(b[i] == a[i]);
	sv_state s0{}, s1{};
	s0.size = s1.size = sizeof s0;
	sv_get_state(none.b, &s0);
	sv_get_state(with.b, &s1);
	CHECK(b[a.size()] >= s0.voices); // (the loaded ones after every compiled-in entry)
	CHECK(s1.voices > s0.voices);
	CHECK(console(with.b, "H").find("H ") != std::string::npos);
}

TEST_CASE("external engines: accordion and trumpet loaded render bit for bit as compiled in, through the board") {
	Board a(NSC_COMPONENT_DIR), x(NSC_COMPONENT_DIR);
	for (const char* id : {"accordion", "trumpet"}) {
		const std::vector<int> e = entries_of(a.b, id);
		REQUIRE(!e.empty());
		REQUIRE(e.size() % 2 == 0);
		const size_t presets = e.size() / 2;
		for (size_t p = 0; p < presets; p++) {
			for (const char* perf : {"default", "stale", id}) {
				std::string err;
				auto pl = sdk::scripted_player(perf, NV, SR, &err);
				if (!pl) continue;
				render_pair(a, e[p], x, e[presets + p], *pl, 4.0, std::string(id) + " preset " + std::to_string(p) + " " + perf);
			}
		}
		for (const auto& f : std::filesystem::directory_iterator(STRISO_REC_DIR)) {
			if (f.path().extension() != ".csv") continue;
			std::string err;
			auto pl = sdk::key_player(f.path().string().c_str(), NV, SR, &err);
			REQUIRE(pl != nullptr);
			render_pair(a, e[0], x, e[presets], *pl, 6.0, std::string(id) + " " + f.path().filename().string());
		}
	}
}
