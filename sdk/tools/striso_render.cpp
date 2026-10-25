// striso_render: any engine and preset, played by a scripted performance, a recorded key stream or
// a MIDI file, to a WAV (and raw float32). It says on stderr exactly which engine, preset and
// input it used (the input file's absolute path, size and checksum), and refuses anything it can't
// find rather than falling back to something else.
//
// Usage: striso_render --engine NAME [--preset N|NAME] [input] [options]
//   input (one of; default --perf default):
//     --perf NAME       a scripted performance compiled in (--list shows them)
//     --rec FILE.csv    a recorded key stream (tools/record_keys.py), as the board plays it
//     --midi FILE.mid   a Standard MIDI File, through the board's MIDI/MPE mapping (striso_dsp/midi_in.h)
//   --seconds S       length (default: the input's: 17.5 s, a recording's length + 2 s)
//   --nv N            voices (default 10, the board's)
//   --rate HZ         sample rate (default: the performance's, 48000)
//   --fx              through the master effects (default parameters): stereo
//   --out FILE.f32    the render as float32 (mono, or L R interleaved)
//   --wav FILE.wav    16 bit WAV, x0.25 and clipped like the board; its INFO chunk names the
//                     engine, preset and input
//   --check           fail (exit 1) on non-finite output, a peak over --max-peak (default 8), or
//                     with --silent-after T any sample above 1e-4 after T s or voices still active
//   --expect HASH     fail unless the render's hash (FNV-1a 64 of the float bits) is HASH
//   --golden-name N   with STRISO_GOLDEN_OUT=<dir>: write the hash to <dir>/N.hash instead
//   --list            the engines, their presets and the performances, and exit
// stdout: "hash <hex>". Exit 2: bad arguments or an input that can't be read.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "striso_sdk/engines.h"
#include "striso_sdk/performance.h"

namespace sdk = striso::sdk;
namespace dsp = striso::dsp;

namespace {

[[noreturn]] void fail(const std::string& msg) {
	std::fprintf(stderr, "striso_render: %s\n", msg.c_str());
	std::exit(2);
}

void list() {
	for (const sdk::EngineEntry* e : sdk::engines()) {
		std::printf("engine %s (%s): %s\n", e->info->name, e->header, e->info->summary);
		for (int i = 0; i < e->info->num_presets; i++) std::printf("  preset %d %s\n", i, e->preset_name(i));
	}
	for (const sdk::ScriptedPerformance* p : sdk::performances()) std::printf("performance %s (%s)\n", p->name, p->header);
}

[[nodiscard]] uint64_t hash_samples(const std::vector<float>& x) {
	uint64_t h = 1469598103934665603ull;
	for (float v : x) {
		uint32_t u;
		std::memcpy(&u, &v, 4);
		for (int k = 0; k < 4; k++) h = (h ^ ((u >> (8 * k)) & 0xffu)) * 1099511628211ull;
	}
	return h;
}

// 16 bit WAV, x0.25 and hard clipped like the board at the usual volume, with a LIST/INFO chunk
[[nodiscard]] bool write_wav(const char* path, const std::vector<float>& x, int channels, int sample_rate, const std::string& comment) {
	FILE* f = std::fopen(path, "wb");
	if (!f) return false;
	const auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	const auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::string info = "INFO";
	const auto sub = [&info](const char* id, const std::string& s) {
		std::string z = s + '\0';
		if (z.size() & 1) z += '\0';
		const uint32_t n = static_cast<uint32_t>(s.size() + 1);
		info += id;
		info.append(reinterpret_cast<const char*>(&n), 4);
		info += z;
	};
	sub("ISFT", "striso_render");
	sub("ICMT", comment);
	const uint32_t n = static_cast<uint32_t>(x.size()), data = n * 2, list = static_cast<uint32_t>(info.size());
	std::fwrite("RIFF", 1, 4, f);
	u32(4 + 8 + 16 + 8 + list + 8 + data);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(1);
	u16(static_cast<uint16_t>(channels));
	u32(static_cast<uint32_t>(sample_rate));
	u32(static_cast<uint32_t>(sample_rate * 2 * channels));
	u16(static_cast<uint16_t>(2 * channels));
	u16(16);
	std::fwrite("LIST", 1, 4, f);
	u32(list);
	std::fwrite(info.data(), 1, info.size(), f);
	std::fwrite("data", 1, 4, f);
	u32(data);
	std::vector<int16_t> s(n);
	for (uint32_t k = 0; k < n; k++) {
		float v = x[k] * 0.25f;
		v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
		s[k] = static_cast<int16_t>(std::lrintf(v * 32767.0f));
	}
	std::fwrite(s.data(), 2, n, f);
	return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv) {
	const char *engine_name = nullptr, *preset_arg = nullptr, *perf = nullptr, *rec = nullptr, *midi = nullptr;
	const char *out_path = nullptr, *wav = nullptr, *expect = nullptr, *golden_name = nullptr;
	double seconds = -1, max_peak = 8.0, silent_after = -1;
	int nv = 10, rate = 0;
	bool use_fx = false, check = false;
	for (int a = 1; a < argc; a++) {
		const std::string s = argv[a];
		const auto next = [&]() -> const char* {
			if (a + 1 >= argc) fail(s + " needs a value");
			return argv[++a];
		};
		if (s == "--engine") engine_name = next();
		else if (s == "--preset") preset_arg = next();
		else if (s == "--perf") perf = next();
		else if (s == "--rec") rec = next();
		else if (s == "--midi") midi = next();
		else if (s == "--seconds") seconds = std::atof(next());
		else if (s == "--nv") nv = std::atoi(next());
		else if (s == "--rate") rate = std::atoi(next());
		else if (s == "--fx") use_fx = true;
		else if (s == "--out") out_path = next();
		else if (s == "--wav") wav = next();
		else if (s == "--check") check = true;
		else if (s == "--max-peak") max_peak = std::atof(next());
		else if (s == "--silent-after") silent_after = std::atof(next());
		else if (s == "--expect") expect = next();
		else if (s == "--golden-name") golden_name = next();
		else if (s == "--list") {
			list();
			return 0;
		} else fail("unknown argument " + s + " (see the usage at the top of sdk/tools/striso_render.cpp)");
	}

	// the engine and the preset: by name, or not at all
	if (!engine_name) fail("no --engine (--list shows them)");
	const sdk::EngineEntry* entry = sdk::find_engine(engine_name);
	if (!entry) {
		std::string known;
		for (const sdk::EngineEntry* e : sdk::engines()) known += std::string(known.empty() ? "" : ", ") + e->info->name;
		fail(std::string("no engine \"") + engine_name + "\" in this program (there are: " + known + ")");
	}
	int preset = -1;
	if (preset_arg) {
		preset = sdk::find_preset(*entry, preset_arg);
		if (preset < 0) fail(std::string("the engine ") + engine_name + " has no preset \"" + preset_arg + "\" (--list shows them)");
	}
	if (nv < 1 || nv > entry->info->budget.max_voices) fail("--nv " + std::to_string(nv) + ": the engine runs 1 to " + std::to_string(entry->info->budget.max_voices) + " voices");

	// the input: exactly one
	if ((perf != nullptr) + (rec != nullptr) + (midi != nullptr) > 1) fail("one input only: --perf, --rec or --midi");
	const bool default_input = !perf && !rec && !midi;
	if (default_input) perf = "default";
	int sr = rate;
	if (!sr) {
		const sdk::ScriptedPerformance* p = sdk::find_performance("default");
		sr = perf && sdk::find_performance(perf) ? sdk::find_performance(perf)->sample_rate : (p ? p->sample_rate : 48000);
	}
	std::string error;
	std::unique_ptr<sdk::Player> player = rec ? sdk::key_player(rec, nv, sr, &error) :
	    midi                                  ? sdk::midi_player(midi, nv, sr, &error) :
	                                            sdk::scripted_player(perf, nv, sr, &error);
	if (!player) fail(error);
	if (seconds < 0) seconds = player->seconds();

	sdk::Engine engine(*entry);
	engine.init(sr, nv);
	if (preset >= 0) engine.set_preset(preset);
	const char* pname = engine.preset() >= 0 ? engine.preset_name(engine.preset()) : nullptr;
	char head[512];
	std::snprintf(head, sizeof head, "engine %s (%s), preset %s%d%s%s%s, %d voices, %d Hz%s", entry->info->name, entry->header,
	    preset_arg ? "" : "(its default) ", engine.preset(), pname ? " \"" : "", pname ? pname : "", pname ? "\"" : "", nv, sr,
	    use_fx ? ", master effects" : "");
	const std::string input = player->describe() + (default_input ? " [the default input: no --perf, --rec or --midi]" : "");
	std::fprintf(stderr, "striso_render: %s\nstriso_render: input %s\n", head, input.c_str());

	sdk::Effect fx(sdk::fx_effect());
	if (use_fx) fx.init(sr);

	const int nblocks = static_cast<int>(seconds * sr / dsp::BLOCK);
	const int channels = use_fx ? 2 : 1;
	std::vector<float> out(static_cast<size_t>(nblocks) * dsp::BLOCK * channels);
	std::vector<dsp::VoiceControls> vc(static_cast<size_t>(nv), dsp::voice_at_rest);
	dsp::GlobalControls g = dsp::globals_at_rest;
	double compute_s = 0;
	for (int b = 0; b < nblocks; b++) {
		player->block(b, vc.data(), g);
		for (dsp::VoiceControls& c : vc) dsp::track_key(c);
		const auto t0 = std::chrono::steady_clock::now();
		float buf[dsp::BLOCK];
		float* const eo[1] = {buf};
		engine.process(vc.data(), g, eo);
		if (use_fx) {
			float l[dsp::BLOCK], r[dsp::BLOCK];
			const float* const in[1] = {buf};
			float* const fo[2] = {l, r};
			fx.process(in, fo);
			for (int n = 0; n < dsp::BLOCK; n++) {
				out[(static_cast<size_t>(b) * dsp::BLOCK + n) * 2] = l[n];
				out[(static_cast<size_t>(b) * dsp::BLOCK + n) * 2 + 1] = r[n];
			}
		} else {
			for (int n = 0; n < dsp::BLOCK; n++) out[static_cast<size_t>(b) * dsp::BLOCK + n] = buf[n];
		}
		compute_s += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	}

	size_t bad = 0, loud_tail = 0;
	float peak = 0;
	const size_t tail_from = silent_after < 0 ? out.size() : static_cast<size_t>(silent_after * sr) * channels;
	for (size_t i = 0; i < out.size(); i++) {
		const float x = out[i];
		if (!std::isfinite(x)) bad++;
		else {
			peak = std::fmax(peak, std::fabs(x));
			if (i >= tail_from && std::fabs(x) > 1e-4f) loud_tail++;
		}
	}
	if (bad) std::fprintf(stderr, "!! %zu NON-FINITE OUTPUT SAMPLES\n", bad);
	if (out_path) {
		FILE* f = std::fopen(out_path, "wb");
		if (!f) fail(std::string("cannot write ") + out_path);
		std::fwrite(out.data(), sizeof(float), out.size(), f);
		std::fclose(f);
		std::fprintf(stderr, "striso_render: wrote %s\n", sdk::file_identity(out_path).c_str());
	}
	if (wav) {
		if (!write_wav(wav, out, channels, sr, std::string(head) + "; input " + input)) fail(std::string("cannot write ") + wav);
		std::fprintf(stderr, "striso_render: wrote %s\n", sdk::file_identity(wav).c_str());
	}
	const uint64_t h = hash_samples(out);
	std::fprintf(stderr, "striso_render: %.1f s audio, host compute %.3f s, peak %.3f\n", seconds, compute_s, peak);
	std::printf("hash %016llx\n", static_cast<unsigned long long>(h));

	int failed = 0;
	if (check) {
		if (bad) failed = 1;
		if (peak > max_peak) {
			std::fprintf(stderr, "!! peak %.3f over %.3f\n", peak, max_peak);
			failed = 1;
		}
		if (silent_after >= 0) {
			const int active = engine.active_voices();
			if (loud_tail) {
				std::fprintf(stderr, "!! %zu samples above 1e-4 after %.2f s\n", loud_tail, silent_after);
				failed = 1;
			}
			if (active) {
				std::fprintf(stderr, "!! %d voices still active at the end\n", active);
				failed = 1;
			}
		}
	}
	const char* golden_out = std::getenv("STRISO_GOLDEN_OUT");
	if (golden_name && golden_out && *golden_out) {
		const std::string path = std::string(golden_out) + "/" + golden_name + ".hash";
		FILE* f = std::fopen(path.c_str(), "w");
		if (!f) fail("cannot write " + path);
		std::fprintf(f, "%s %016llx\n", golden_name, static_cast<unsigned long long>(h));
		std::fclose(f);
		expect = nullptr;
	}
	if (expect) {
		char hs[32];
		std::snprintf(hs, sizeof hs, "%016llx", static_cast<unsigned long long>(h));
		if (std::strcmp(hs, expect) != 0) {
			std::fprintf(stderr, "!! render hash %s, expected %s (golden render changed)\n", hs, expect);
			failed = 1;
		}
	}
	return failed;
}
