// MIDI in -> voice controls on the PC (midi_voices.h).
#include "striso_sdk/midi_voices.h"

#include <algorithm>

namespace striso::sdk {

MidiVoices::MidiVoices(int voices) :
    vc_(static_cast<size_t>(voices), dsp::voice_at_rest), g_(dsp::globals_at_rest), voice_(static_cast<size_t>(voices)) {
	std::fill(std::begin(slot_voice_), std::end(slot_voice_), -1);
}

int MidiVoices::notes_held() const {
	int n = 0;
	for (int s = 0; s < midi::NOTES; s++) n += in_.channel(s) >= 0;
	return n;
}

bool MidiVoices::message(const uint8_t* m, int len) {
	if (len < 1 || m[0] < 0x80 || m[0] >= 0xf0) return true; // (system messages: nothing to play)
	if (queued_ == QUEUE) return false;
	queue_[queued_][0] = m[0];
	queue_[queued_][1] = len > 1 ? m[1] & 0x7f : 0;
	queue_[queued_][2] = len > 2 ? m[2] & 0x7f : 0;
	queued_++;
	return true;
}

void MidiVoices::step() {
	if (in_.fading()) in_.tick(*this);
	for (int i = 0; i < queued_; i++) apply(queue_[i][0], queue_[i][1], queue_[i][2]);
	queued_ = 0;
}

// as the board's midi_in_message (synth_control.cpp) for what a sound needs
void MidiVoices::apply(uint8_t status, uint8_t d1, uint8_t d2) {
	const uint8_t channel = status & 0x0f;
	status &= 0xf0;
	if (in_.message(*this, channel, status, d1, d2)) return;
	if (status == 0xb0) {
		if (rpn_.control(channel, d1, d2)) return;
		if (d1 == 6 && rpn_.is(channel, 0, 0)) {
			// the notes' bend range, at least 1; an MPE zone's master channel's is not the notes'
			if (d2 >= 1 && !(channel == 0 && in_.members() > 0)) bend_range_ = d2;
		} else if (d1 == 6 && rpn_.is(channel, 0, 6)) {
			if (channel == 0 && d2 <= 15) in_.set_members(d2); // MCM: the lower zone
		} else if (d1 == 64) {
			g_.pedal = static_cast<float>(d2) * (1.0f / 127.0f);
		}
	} else if (status == 0xe0) {
		in_.set_bend(*this, midi::instrument_bend(d1, d2)); // the whole instrument, +-2 semitones
	}
}

// a voice for slot s: never used (the lowest), else released longest ago, else none
void MidiVoices::note_started(int s, const midi::NoteSignals&) {
	int v = slot_voice_[s];
	if (v < 0 || voice_[static_cast<size_t>(v)].slot != s) {
		v = -1;
		int released = -1;
		for (int n = 0; n < num_voices() && v < 0; n++) {
			const Voice& x = voice_[static_cast<size_t>(n)];
			if (x.slot < 0 && x.released == 0) v = n;
			else if (!x.held && (released < 0 || x.released < voice_[static_cast<size_t>(released)].released)) released = n;
		}
		if (v < 0) v = released;
	}
	if (v >= 0) {
		const int old = voice_[static_cast<size_t>(v)].slot;
		if (old >= 0 && old != s) slot_voice_[old] = -1;
		voice_[static_cast<size_t>(v)] = {s, true, 0};
	}
	slot_voice_[s] = v;
}

void MidiVoices::note_changed(int s, const midi::NoteSignals& n) {
	const int v = slot_voice_[s];
	if (v < 0 || voice_[static_cast<size_t>(v)].slot != s) return;
	dsp::VoiceControls& c = vc_[static_cast<size_t>(v)];
	c.note = n.note;
	c.pres = n.pres;
	c.vpres = n.vpres;
	c.but_x = n.but_x;
	c.but_y = std::clamp(n.but_y, -1.0f, 1.0f);
	Voice& x = voice_[static_cast<size_t>(v)];
	if (x.held && !n.on) x.released = ++clock_;
	x.held = n.on;
}

void MidiVoices::note_silenced(int s) {
	const int v = slot_voice_[s];
	if (v < 0 || voice_[static_cast<size_t>(v)].slot != s) return;
	vc_[static_cast<size_t>(v)].pres = 0.0f;
	vc_[static_cast<size_t>(v)].vpres = 0.0f;
}

void MidiVoices::note_dropped(int s) {
	note_silenced(s);
	const int v = slot_voice_[s];
	if (v >= 0 && voice_[static_cast<size_t>(v)].slot == s && voice_[static_cast<size_t>(v)].held) {
		voice_[static_cast<size_t>(v)].held = false;
		voice_[static_cast<size_t>(v)].released = ++clock_;
	}
}

} // namespace striso::sdk
