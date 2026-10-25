// MIDI and MPE in -> VoiceControls (midi_in.h): the board's mapping, src/synth/synth_control.cpp
// Instrument::midi_in_* and midi_in_message().
#include "striso_sdk/midi_in.h"

#include <algorithm>

namespace striso::sdk {

namespace {

// a held note's pres stays above 0 (pres 0 is a release for the engines)
[[nodiscard]] float held_pres(float p) { return std::max(p, 1.0f / 127.0f); }

} // namespace

MidiIn::MidiIn(int voices) :
    vc_(static_cast<size_t>(voices), dsp::voice_at_rest), voice_(static_cast<size_t>(voices)), g_(dsp::globals_at_rest) {
	std::fill(std::begin(rpn_lsb_), std::end(rpn_lsb_), uint8_t{0x7f});
	std::fill(std::begin(rpn_msb_), std::end(rpn_msb_), uint8_t{0x7f});
}

int MidiIn::notes_held() const {
	return static_cast<int>(std::count_if(std::begin(slot_), std::end(slot_), [](const Slot& s) { return s.channel >= 0; }));
}

int MidiIn::find(int channel, int key) const {
	for (int s = 0; s < SLOTS; s++)
		if (slot_[s].channel == channel && slot_[s].key == key) return s;
	return -1;
}

// a voice for slot s: the one it has, else a voice never used, else the longest released one,
// else the held one with the least pressure, if that is less than the new note's (else none)
int MidiIn::take_voice(int s) {
	const int nv = num_voices();
	int v = slot_[s].voice;
	if (v >= 0 && voice_[v].slot == s) { // (a note struck again, or a slot taken again)
		voice_[v] = {s, true, 0};
		return v;
	}
	v = -1;
	int released = -1, held = -1;
	float min_held = slot_[s].pres + slot_[s].vpres;
	for (int n = 0; n < nv && v < 0; n++) {
		const Voice& x = voice_[n];
		if (x.slot < 0 && x.released == 0) v = n;
		else if (!x.held) {
			if (released < 0 || x.released < voice_[released].released) released = n;
		} else if (vc_[n].pres < min_held) {
			min_held = vc_[n].pres;
			held = n;
		}
	}
	if (v < 0) v = released >= 0 ? released : held;
	if (v < 0) return -1;
	if (voice_[v].slot >= 0 && voice_[v].slot != s) slot_[voice_[v].slot].voice = -1; // stolen
	voice_[v] = {s, true, 0};
	slot_[s].voice = v;
	return v;
}

// the slot's note and controls into its voice
void MidiIn::write(int s) {
	Slot& m = slot_[s];
	const int v = m.voice;
	if (v < 0 || voice_[v].slot != s) return;
	dsp::VoiceControls& c = vc_[static_cast<size_t>(v)];
	if (m.channel >= 0) {
		c.note = static_cast<float>(m.key) + master_bend_ + (member(m.channel) ? chan_bend_[m.channel] : 0.0f);
		c.but_y = std::clamp(chan_y_[m.channel], -1.0f, 1.0f);
	}
	c.pres = m.pres;
	c.vpres = m.vpres;
	c.but_x = 0.0f;
}

void MidiIn::note_on(int channel, int key, int velo) {
	int s = find(channel, key);
	for (int n = 0; s < 0 && n < SLOTS; n++)
		if (slot_[n].channel < 0) s = n;
	if (s < 0) { // all in use: the oldest, silenced
		s = 0;
		for (int n = 1; n < SLOTS; n++)
			if (slot_[n].age < slot_[s].age) s = n;
		if (slot_[s].voice >= 0 && voice_[slot_[s].voice].slot == s) {
			dsp::VoiceControls& c = vc_[static_cast<size_t>(slot_[s].voice)];
			c.pres = c.vpres = 0.0f;
			voice_[slot_[s].voice] = {-1, false, ++age_};
		}
		slot_[s].voice = -1;
	}
	Slot& m = slot_[s];
	m.channel = static_cast<int8_t>(channel);
	m.key = static_cast<uint8_t>(key);
	m.age = ++age_;
	m.pressure = chan_pres_set_[channel]; // MPE: the pressure sent before the note-on
	m.vpres = static_cast<float>(velo) * (1.0f / 127.0f);
	m.pres = held_pres(m.pressure ? chan_pres_[channel] : m.vpres);
	const int v = take_voice(s);
	if (v < 0) return; // every voice plays a louder held note
	dsp::VoiceControls& c = vc_[static_cast<size_t>(v)];
	c.gate = 1.0f;
	c.strike = m.vpres;
	write(s);
}

void MidiIn::note_off(int channel, int key, int velo) {
	const int s = find(channel, key);
	if (s < 0) return;
	Slot& m = slot_[s];
	m.pres = 0.0f;
	m.vpres = -static_cast<float>(velo) * (1.0f / 127.0f);
	write(s);
	if (m.voice >= 0 && voice_[m.voice].slot == s) {
		dsp::VoiceControls& c = vc_[static_cast<size_t>(m.voice)];
		c.gate = 0.0f;
		c.release = -m.vpres;
		voice_[m.voice].held = false;
		voice_[m.voice].released = ++age_;
	}
	m.channel = -1;
	bool any = false;
	for (const Slot& x : slot_) any |= x.channel == channel;
	if (!any) chan_pres_set_[channel] = false; // pressure is per note (MPE)
}

// pressure 0..127 of the notes of a channel (key -1: channel pressure)
void MidiIn::pressure(int channel, int key, int value) {
	const float p = static_cast<float>(value) * (1.0f / 127.0f);
	if (key < 0) {
		chan_pres_[channel] = p;
		chan_pres_set_[channel] = true;
	}
	for (int s = 0; s < SLOTS; s++)
		if (slot_[s].channel == channel && (key < 0 || slot_[s].key == key)) {
			slot_[s].pressure = true;
			slot_[s].pres = held_pres(p);
			write(s);
		}
}

// rewrite the notes of a channel (-1: all) after a change of its bend or y
void MidiIn::update(int channel) {
	for (int s = 0; s < SLOTS; s++)
		if (slot_[s].channel >= 0 && (channel < 0 || slot_[s].channel == channel)) write(s);
}

void MidiIn::tick() {
	for (int s = 0; s < SLOTS; s++)
		if (slot_[s].channel >= 0 && slot_[s].vpres > 0.0f) {
			slot_[s].vpres = slot_[s].vpres > 0.01f ? slot_[s].vpres * 0.6f : 0.0f;
			write(s);
		}
}

void MidiIn::all_off() {
	for (int s = 0; s < SLOTS; s++) {
		if (slot_[s].channel >= 0 && slot_[s].voice >= 0 && voice_[slot_[s].voice].slot == s) {
			dsp::VoiceControls& c = vc_[static_cast<size_t>(slot_[s].voice)];
			c.pres = c.vpres = 0.0f;
			c.gate = 0.0f;
			voice_[slot_[s].voice] = {-1, false, ++age_};
		}
		slot_[s] = Slot();
	}
	std::fill(std::begin(chan_pres_set_), std::end(chan_pres_set_), false);
}

void MidiIn::control_change(int channel, int cc, int value) {
	switch (cc) {
	case 1: // mod wheel, 0..1
		chan_y_[channel] = static_cast<float>(value) * (1.0f / 127.0f);
		update(channel);
		break;
	case 74: // timbre, 64 = 0
		chan_y_[channel] = std::clamp(static_cast<float>(value - 64) * (1.0f / 64.0f), -1.0f, 1.0f);
		update(channel);
		break;
	case 64: g_.pedal = static_cast<float>(value) * (1.0f / 127.0f); break;
	case 100: rpn_lsb_[channel] = static_cast<uint8_t>(value); break;
	case 101: rpn_msb_[channel] = static_cast<uint8_t>(value); break;
	case 98:
	case 99: rpn_lsb_[channel] = rpn_msb_[channel] = 0x7f; break; // an NRPN deselects the RPN
	case 6:                                                       // data entry
		if (rpn_lsb_[channel] == 0 && rpn_msb_[channel] == 0) {
			// the notes' bend range; an MPE zone's master channel's is not the notes' (at least 1)
			if (value >= 1 && !(channel == 0 && members_ > 0)) bend_range_ = static_cast<float>(value);
		} else if (rpn_lsb_[channel] == 6 && rpn_msb_[channel] == 0) {
			if (channel == 0 && value <= 15) members_ = value; // MCM: the lower zone
		}
		break;
	case 120: // all sound off
	case 123: // all notes off
		all_off();
		break;
	default: break;
	}
}

void MidiIn::message(const uint8_t* m, int len) {
	if (len < 1 || m[0] < 0x80 || m[0] >= 0xf0) return;
	const int status = m[0] & 0xf0, channel = m[0] & 0x0f;
	const int d1 = len > 1 ? m[1] & 0x7f : 0, d2 = len > 2 ? m[2] & 0x7f : 0;
	switch (status) {
	case 0x80: note_off(channel, d1, d2); break;
	case 0x90:
		if (d2 == 0) note_off(channel, d1, 0);
		else note_on(channel, d1, d2);
		break;
	case 0xa0: pressure(channel, d1, d2); break;
	case 0xd0: pressure(channel, -1, d1); break;
	case 0xe0: {
		const float b = static_cast<float>(((d2 << 7) | d1) - 0x2000) * (1.0f / 8192);
		if (member(channel)) {
			chan_bend_[channel] = b * bend_range_;
			update(channel);
		} else {
			master_bend_ = 2.0f * b; // the whole instrument, +-2 semitones
			update(-1);
		}
	} break;
	case 0xb0: control_change(channel, d1, d2); break;
	default: break;
	}
}

} // namespace striso::sdk
