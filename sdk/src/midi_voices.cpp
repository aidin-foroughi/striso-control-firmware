// MIDI in -> voice controls on the PC (midi_voices.h): the board's units, this host's controls.
#include "striso_sdk/midi_voices.h"

#include <algorithm>

namespace striso::sdk {

MidiVoices::MidiVoices(int voices) :
    vc_(static_cast<size_t>(voices), dsp::voice_at_rest), g_(dsp::globals_at_rest) {
	alloc_.set_voices(voices);
}

int MidiVoices::notes_held() const {
	int n = 0;
	for (int s = 0; s < midi::NOTES; s++) n += in_.channel(s) >= 0;
	return n;
}

bool MidiVoices::message(const uint8_t* m, int len) {
	if (len < 1 || m[0] < 0x80 || m[0] >= 0xf0) return true; // (system messages: nothing to play)
	if (queued_ == QUEUE) return false;
	queue_[queued_][0] = (uint32_t)m[0] << 16 | (uint32_t)(len > 1 ? m[1] & 0x7f : 0) << 8 | (uint32_t)(len > 2 ? m[2] & 0x7f : 0);
	queue_[queued_][1] = 0xffffffffu;
	queued_++;
	return true;
}

bool MidiVoices::ump2(uint32_t w0, uint32_t w1) {
	if ((w0 >> 28) != 4) return true; // (only MIDI 2.0 channel voice messages play)
	if (queued_ == QUEUE) return false;
	queue_[queued_][0] = w0;
	queue_[queued_][1] = w1;
	queued_++;
	return true;
}

// as the board's main loop: synth_tick (the level decay, the strike fade), then the messages
void MidiVoices::step() {
	alloc_.tick(*this);
	if (dec_.fading()) dec_.tick(*this);
	for (int i = 0; i < queued_; i++) {
		const uint32_t w0 = queue_[i][0], w1 = queue_[i][1];
		if (w1 == 0xffffffffu && (w0 >> 24) == 0) apply((uint8_t)(w0 >> 16), (uint8_t)(w0 >> 8), (uint8_t)w0);
		else if (!in_.message2(*this, w0, w1)) (void)instrument_.message2(*this, in_, w0, w1);
		dec_.flush(*this);
	}
	queued_ = 0;
}

void MidiVoices::apply(uint8_t status, uint8_t d1, uint8_t d2) {
	const uint8_t channel = status & 0x0f;
	status &= 0xf0;
	if (in_.message(*this, channel, status, d1, d2)) return;
	(void)instrument_.message(*this, in_, channel, status, d1, d2);
}

// as the board's Instrument::note_started: held, its level, a voice
void MidiVoices::note_started(int s, const midi::NoteSignals& n) {
	held_[s] = true;
	alloc_.set_level(s, n.pres + n.vpres); // a new note: no old estimate
	(void)alloc_.start(*this, s);
}

void MidiVoices::note_changed(int s, const midi::NoteSignals& n) {
	held_[s] = n.on;
	floor_[s] = n.on ? n.pres + n.vpres : -1.0f;
	alloc_.raise_level(s, floor_[s]);
	const int v = alloc_.voice(s);
	if (v < 0) return;
	dsp::VoiceControls& c = vc_[static_cast<size_t>(v)];
	c.note = n.note;
	c.pres = n.pres;
	c.vpres = n.vpres;
	c.but_x = n.but_x;
	c.but_y = std::clamp(n.but_y, -1.0f, 1.0f);
}

void MidiVoices::note_silenced(int s) {
	const int v = alloc_.voice(s);
	if (v < 0) return;
	vc_[static_cast<size_t>(v)].pres = 0.0f;
	vc_[static_cast<size_t>(v)].vpres = 0.0f;
}

void MidiVoices::note_dropped(int s) {
	note_silenced(s);
	held_[s] = false;
	floor_[s] = -1.0f;
}

} // namespace striso::sdk
