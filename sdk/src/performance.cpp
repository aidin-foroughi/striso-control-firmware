// The players (performance.h).
#include "striso_sdk/performance.h"

#include <cstdio>
#include <filesystem>
#include <vector>

#include "harness_util.h" // tools/harness: load_recording, the board's replay of a key stream
#include "striso_sdk/midi_voices.h"
#include "striso_sdk/smf.h"

namespace striso::sdk {

const ScriptedPerformance* find_performance(std::string_view name) {
	for (const ScriptedPerformance* p : performances())
		if (name == p->name) return p;
	return nullptr;
}

std::string file_identity(const char* path) {
	std::error_code ec;
	const std::filesystem::path abs = std::filesystem::weakly_canonical(std::filesystem::absolute(path, ec), ec);
	FILE* f = std::fopen(path, "rb");
	if (!f) return abs.string() + " (unreadable)";
	uint64_t h = 1469598103934665603ull, n = 0;
	int c;
	while ((c = std::fgetc(f)) != EOF) {
		h = (h ^ static_cast<uint8_t>(c)) * 1099511628211ull;
		n++;
	}
	std::fclose(f);
	char tail[64];
	std::snprintf(tail, sizeof tail, " (%llu bytes, fnv %016llx)", static_cast<unsigned long long>(n), static_cast<unsigned long long>(h));
	return abs.string() + tail;
}

namespace {

class Scripted final : public Player {
public:
	Scripted(const ScriptedPerformance& p, int voices) :
	    p_(p), nv_(voices) {}
	void block(int b, dsp::VoiceControls* vc, dsp::GlobalControls& g) override { p_.block(b, nv_, vc, g); }
	[[nodiscard]] double seconds() const override { return 17.5; }
	[[nodiscard]] std::string describe() const override {
		return std::string("scripted performance \"") + p_.name + "\" (" + file_identity(p_.header) + ")";
	}

private:
	const ScriptedPerformance& p_;
	int nv_;
};

class Keys final : public Player {
public:
	Keys(std::string path, std::vector<harness::RecEvent> ev, int sample_rate) :
	    path_(std::move(path)), ev_(std::move(ev)), sr_(sample_rate) {}
	void block(int b, dsp::VoiceControls* vc, dsp::GlobalControls&) override {
		// the last event of each voice up to this block (the board's controls between messages)
		const double t = static_cast<double>(b) * dsp::BLOCK / sr_;
		for (; k_ < ev_.size() && ev_[k_].t <= t; k_++) {
			const harness::RecEvent& x = ev_[k_];
			dsp::VoiceControls& c = vc[x.voice];
			c.note = x.note;
			c.pres = x.pres;
			c.vpres = x.vpres;
			c.but_x = x.but_x;
			c.but_y = x.but_y;
		}
	}
	[[nodiscard]] double seconds() const override { return ev_.back().t + 2.0; }
	[[nodiscard]] std::string describe() const override {
		char tail[96];
		std::snprintf(tail, sizeof tail, ": %zu key events, %.1f s", ev_.size(), ev_.back().t);
		return "key recording " + file_identity(path_.c_str()) + tail;
	}

private:
	std::string path_;
	std::vector<harness::RecEvent> ev_;
	int sr_;
	size_t k_ = 0;
};

class Midi final : public Player {
public:
	Midi(std::string path, MidiFile f, int voices, int sample_rate) :
	    path_(std::move(path)), f_(std::move(f)), in_(voices), sr_(sample_rate) {}
	void block(int b, dsp::VoiceControls* vc, dsp::GlobalControls& g) override {
		// the 2 ms MIDI steps due by this block (as the board takes MIDI in), each with the
		// messages that came before it
		const double t = static_cast<double>(b) * dsp::BLOCK / sr_;
		for (; static_cast<double>(steps_) * midi::TICK_SECONDS <= t; steps_++) {
			const double ts = static_cast<double>(steps_) * midi::TICK_SECONDS;
			for (; k_ < f_.events.size() && f_.events[k_].t <= ts; k_++) (void)in_.message(f_.events[k_].bytes, f_.events[k_].len);
			in_.step();
		}
		for (int v = 0; v < in_.num_voices(); v++) {
			const dsp::VoiceControls& m = in_.voices()[v];
			vc[v].note = m.note;
			vc[v].pres = m.pres;
			vc[v].vpres = m.vpres;
			vc[v].but_x = m.but_x;
			vc[v].but_y = m.but_y;
		}
		g = in_.globals();
	}
	[[nodiscard]] double seconds() const override { return f_.seconds + 2.0; }
	[[nodiscard]] std::string describe() const override {
		char tail[128];
		std::snprintf(tail, sizeof tail, ": format %d, %d tracks, %zu channel messages, %.1f s", f_.format, f_.tracks, f_.events.size(), f_.seconds);
		return "MIDI file " + file_identity(path_.c_str()) + tail;
	}

private:
	std::string path_;
	MidiFile f_;
	MidiVoices in_;
	int sr_;
	size_t k_ = 0;
	uint64_t steps_ = 0;
};

} // namespace

std::unique_ptr<Player> scripted_player(std::string_view name, int voices, int, std::string* error) {
	const ScriptedPerformance* p = find_performance(name);
	if (!p) {
		std::string known;
		for (const ScriptedPerformance* q : performances()) known += std::string(known.empty() ? "" : ", ") + q->name;
		*error = "no scripted performance \"" + std::string(name) + "\" (there are: " + known + ")";
		return nullptr;
	}
	if (voices > p->max_voices) {
		*error = "the performance \"" + std::string(name) + "\" has at most " + std::to_string(p->max_voices) + " voices";
		return nullptr;
	}
	return std::make_unique<Scripted>(*p, voices);
}

std::unique_ptr<Player> key_player(const char* csv_path, int voices, int sample_rate, std::string* error) {
	if (FILE* f = std::fopen(csv_path, "rb")) std::fclose(f);
	else {
		*error = std::string("cannot read the key recording ") + csv_path;
		return nullptr;
	}
	std::vector<harness::RecEvent> ev = harness::load_recording(csv_path, voices);
	if (ev.empty()) {
		*error = std::string("no key events in ") + csv_path;
		return nullptr;
	}
	return std::make_unique<Keys>(csv_path, std::move(ev), sample_rate);
}

std::unique_ptr<Player> midi_player(const char* mid_path, int voices, int sample_rate, std::string* error) {
	MidiFile f = read_midi_file(mid_path);
	if (!f.error.empty()) {
		*error = f.error + " (" + mid_path + ")";
		return nullptr;
	}
	if (f.events.empty()) {
		*error = std::string("no channel messages in ") + mid_path;
		return nullptr;
	}
	return std::make_unique<Midi>(mid_path, std::move(f), voices, sample_rate);
}

} // namespace striso::sdk
