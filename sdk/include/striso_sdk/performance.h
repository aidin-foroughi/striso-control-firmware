// What plays an engine: a Player gives the controls of every block. Three kinds:
//   scripted   a performance compiled in from tools/harness (performance.h: "default",
//              perf_stale.h: "stale", perf_<name>.h: "<name>"), deterministic
//   keys       a recorded key stream (tools/record_keys.py CSV), replayed as the board's
//              synth_control would play it (tools/harness/harness_util.h: load_recording)
//   midi       a Standard MIDI File through the MIDI/MPE mapping (midi_in.h)
// Each says what it is and where it came from (describe()), for the renderer's log.
#ifndef STRISO_SDK_PERFORMANCE_H_
#define STRISO_SDK_PERFORMANCE_H_

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "striso_dsp/engine.h"

namespace striso::sdk {

// a scripted performance, compiled in (sdk/CMakeLists.txt: one per header)
struct ScriptedPerformance {
	const char* name;   // "default", "stale", "bowed", ...
	const char* header; // the file it was compiled from
	int sample_rate;    // the rate it is written for (its SAMPLERATE)
	int max_voices;     // its NVOICES_MAX
	// the key signals (note, pres, vpres, but_x, but_y) of block b for nv voices, and the globals
	void (*block)(int b, int nv, dsp::VoiceControls* vc, dsp::GlobalControls& g);
};

// the scripted performances linked in (striso_performances), by name
[[nodiscard]] std::span<const ScriptedPerformance* const> performances();
[[nodiscard]] const ScriptedPerformance* find_performance(std::string_view name);

class Player {
public:
	virtual ~Player() = default;
	// the controls of block b (blocks in order from 0): writes what changes, the rest stays
	virtual void block(int b, dsp::VoiceControls* vc, dsp::GlobalControls& g) = 0;
	// its natural length in seconds (a render's default)
	[[nodiscard]] virtual double seconds() const = 0;
	// what it is, for a log: kind, name, file (absolute), its size and checksum, its length
	[[nodiscard]] virtual std::string describe() const = 0;
	// true: it sets gate, strike and release itself (MIDI); false: they follow from the key
	// signals (dsp::track_key, as on the board)
	[[nodiscard]] virtual bool note_events() const { return false; }
};

// nullptr and a reason in *error if it can't play (unknown name, unreadable file, no events)
[[nodiscard]] std::unique_ptr<Player> scripted_player(std::string_view name, int voices, int sample_rate, std::string* error);
[[nodiscard]] std::unique_ptr<Player> key_player(const char* csv_path, int voices, int sample_rate, std::string* error);
[[nodiscard]] std::unique_ptr<Player> midi_player(const char* mid_path, int voices, int sample_rate, std::string* error);

// the file's absolute path, size and FNV-1a 64 checksum, "<path> (<n> bytes, fnv <hex>)"
[[nodiscard]] std::string file_identity(const char* path);

} // namespace striso::sdk

#endif // STRISO_SDK_PERFORMANCE_H_
