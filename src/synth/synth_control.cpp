/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
#include <math.h>
#include <string.h>

#include "ch.h"
#include "hal.h"

#include "ccportab.h"

extern "C" {
    #include "synth.h"
    #include "ws2812.h"
    #include "led.h"
    #include "aux_jack.h"
    #include "button_read.h"
}

#include "config.h"
#include "config_store.h"
#include "config_table.hpp"
#include "preset_loader.hpp"
#include "settings_keys.hpp"
#include "led_view.hpp"
#include "striso.h"
#include "midi_usb.h"
#include "midi_serial.h"
#include "midi.h"
#include "tuning.h"
#include "chprintf.h"
#include "messaging.hpp"
#include "striso_dsp/midi_in.h"
#include "striso_dsp/ump_out.h"
#include "ump_stream.hpp"
#include "striso_dsp/voice_allocator.h"

#include <array>
#include <bit>
#include <cstdint>
#include <type_traits>

#ifndef USE_WS2812
#define ws2812_write_led(n,r,g,b) led_rgb3(14*r,14*g,14*b)
#endif

#define BUTTONCOUNT 68
// notes from MIDI in: Button slots BUTTONCOUNT.. (they share the internal synth voices with the keys)
#define MIDI_IN_NOTES 16
static_assert(MIDI_IN_NOTES == striso::midi::NOTES, "a Button slot per MIDI in note");
#define MAX_PORTAMENTO_BUTTONS 8
static constexpr uint8_t MIDI_C_ALL_SOUND_OFF = 0x78;  // (midi.h has All Notes Off, 0x7b)

// the estimated volume's decay per tick (the MIDI voices'; the internal synth's: voice_allocator.h)
#define VOL_TICK (striso::voices::LEVEL_STEP)
#define VOL_TICK_FACT (striso::voices::LEVEL_DECAY)

float volume_linear = 90.0f; // volume in range 0-127
void set_volume(float vol) {
    volume_linear = vol;
    volume = volume_linear * volume_linear * (1.0f / 16129.0f);
}

static bool note_channel(int channel);
static bool note_cc(int cc);

// MIDI mode midi2 on USB's MIDI 2.0 alt setting: the keys' notes go out as MIDI 2.0 (MidiSendUmp),
// so MIDI 1.0's note data (MPE's notes, pressure, bends and note CCs on the note channels) stays
// off USB; DIN, and USB's MIDI 1.0 alt setting, get all of it (doc/engines.md, "MIDI 2.0 out")
static bool usb_note_data_dropped(uint8_t b0, uint8_t b1) {
    if (!config.midi2 || !midi_usb_ump()) return false;
    const uint8_t s = b0 & 0xf0;
    const int channel = b0 & 0x0f;
    if (s == 0x80 || s == 0x90 || s == 0xa0 || s == 0xd0 || s == 0xe0) return note_channel(channel);
    return s == 0xb0 && b1 < 120 && note_channel(channel) && note_cc(b1);
}

void MidiSend1(uint8_t b0) {
    midi_usb_MidiSend1(1, b0);
#ifdef USE_MIDI_SERIAL
    serial_MidiSend1(b0);
#endif
}

// When the keys last sent note data (note on/off, pressure, pitch bend) on each channel (+1; 0:
// never): MIDI note input on such a channel within 0.5 s is the Striso's own output echoed back
// by a DAW's MIDI thru or a loop (SC-16), not notes to play.
static systime_t note_out_at[16];
static void mark_note_out(uint8_t b0) {
    uint8_t s = b0 & 0xf0;
    if (s == 0x80 || s == 0x90 || s == 0xa0 || s == 0xd0 || s == 0xe0) {
        note_out_at[b0 & 0x0f] = chVTGetSystemTime() + 1;
    }
}
static bool note_out_recently(int channel) {
    systime_t t = note_out_at[channel & 0x0f];
    return t != 0 && (systime_t)(chVTGetSystemTime() - (t - 1)) < TIME_MS2I(500);
}

// (data bytes are 7 bit: a byte with bit 7 set would be read as a status byte)
void MidiSend2(uint8_t b0, uint8_t b1) {
    mark_note_out(b0);
    b1 &= 0x7f;
    if (!usb_note_data_dropped(b0, b1)) midi_usb_MidiSend2(1, b0, b1);
#ifdef USE_MIDI_SERIAL
    serial_MidiSend2(b0, b1);
#endif
}

void MidiSend3(uint8_t b0, uint8_t b1, uint8_t b2) {
    mark_note_out(b0);
    b1 &= 0x7f;
    b2 &= 0x7f;
    if (!usb_note_data_dropped(b0, b1)) midi_usb_MidiSend3(1, b0, b1, b2);
#ifdef USE_MIDI_SERIAL
    serial_MidiSend3(b0, b1, b2);
#endif
}

// Schlick power function, approximation of power function
float powf_schlick(const float a, const float b)
{
    return (a / (b - a * b + a));
}

// Derivative of schlick power function
float powf_schlick_d(const float a, const float b)
{
    float t = (-a*b+a+b);
    return b/(t*t);
}

#define pow2(x) ((x)*(x))
#define pow3(x) ((x)*(x)*(x))
#define max(x, y) ((x)>(y)?(x):(y))
#define min(x, y) ((x)<(y)?(x):(y))
#define clamp(x, low, high)  (((x) > (high)) ? (high) : (((x) < (low)) ? (low) : (x)))

float clamp_rem(float x, float low, float high, float* rem) {
    if (x > high) {
        *rem = x - high;
        return high;
    } else if (x < low) {
        *rem = x - low;
        return low;
    } else {
        return x;
    }
}

// a settings knob's new value: clamp_rem to the knob's range, widened to the current value, so a
// value set outside that range (by a preset or Striso Live) moves back towards it but never jumps
// against the direction of the knob (SC-13)
float clamp_knob(float cur, float x, float low, float high, float* rem) {
    return clamp_rem(x, min(low, cur), max(high, cur), rem);
}

// float to int, limited to lo..hi first: an out of range (or NaN) float to int is UB, and on
// the board it gives the wrong end of the range (an absurd tuning, a bend range of 0)
static inline int to_int_clamped(double x, int lo, int hi) {
    if (!(x >= lo)) return lo;  // (also NaN)
    if (x > hi) return hi;
    return (int)x;
}

// time a is after time b; the system time wraps (32 bit at 2 kHz: after 24.9 days)
static inline bool time_after(systime_t a, systime_t b) {
    return (std::make_signed_t<systime_t>)(a - b) > 0;
}

void update_leds(void);
static void midi_in_message(uint8_t status, uint8_t data1, uint8_t data2);
static void midi_in_message2(uint32_t w0, uint32_t w1);
void set_midi_mode(midi_mode_t mode);
void set_midi2(void);
float config_but(int but, int type, float adjust);
static volatile bool reset_on_release = false;  // settings + key 67, see synth_tick()

typedef enum {
    STATE_OFF = 0,
    STATE_ON = 1,
    STATE_PORTAMENTO = 2,
    STATE_ALT = 3,
    STATE_TRANSPOSE = 4,
} button_state_t;

/*
 * Send midi messages with hysteresis filtering
 */
class MidiParam {
    public:
        int last_value = -1; // nothing sent yet (INT32_MAX overflowed in last_value*4 below)
        int midi_cc = CFG_DISABLE;

        MidiParam(int cc) {
            midi_cc = cc;
        }

        /*
         * Send midi message if more than 0.75 different from last value
         */
        void send(int value_x4, int midi_channel) {
            int d = (value_x4 < last_value*4) * 2 + 1; // calculate direction for hysteresis
            int value = __USAT((value_x4 + d) >> 2, 7) & 0x7F;
            if (value != last_value) {
                if (midi_cc < 120) {
                    MidiSend3(MIDI_CONTROL_CHANGE | midi_channel,
                                    midi_cc, value);
                } else if (midi_cc == CFG_CHANNEL_PRESSURE) {
                    MidiSend2(MIDI_CHANNEL_PRESSURE | midi_channel,
                                    value);
                }
                last_value = value;
            }
        }
};

class MotionSensor {
    public:
        // when each value of CC 16, 17, 18 was last sent (+1; 0: never), to spot an echo (SC-16)
        systime_t sent_at[3][128] = {};
        bool sent_recently(int n, int value) const {
            systime_t t = sent_at[n][value & 0x7f];
            return t != 0 && (systime_t)(chVTGetSystemTime() - (t - 1)) < TIME_MS2I(500);
        }
        int send_motion_time = 0;
        int last_acc_x = INT32_MAX;
        int last_acc_y = INT32_MAX;
        int last_acc_z = INT32_MAX;
        int last_acc_abs = INT32_MAX;
        int last_rot_x = INT32_MAX;
        int last_rot_y = INT32_MAX;
        int last_rot_z = INT32_MAX;

        MotionSensor() {}

        void message(int* msg) {
            int acc_x = msg[0];
            int acc_y = msg[1];
            int acc_z = msg[2];
            int acc_abs = msg[3];
            int rot_x = msg[4];
            int rot_y = msg[5];
            int rot_z = msg[6];
#ifdef USE_MIDI_OUT
            if (config.send_motion_interval && (--send_motion_time <= 0)) {
                send_motion_time = config.send_motion_interval;
                if (config.send_motion_14bit) {
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    16|MIDI_C_LSB, acc_x&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    16, (64+(acc_x>>7))&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    17|MIDI_C_LSB, acc_y&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    17, (64+(acc_y>>7))&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    18|MIDI_C_LSB, acc_z&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    18, (64+(acc_z>>7))&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    19|MIDI_C_LSB, acc_abs&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    19, (acc_abs>>6)&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    80|MIDI_C_LSB, rot_x&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    80, (64+(rot_x>>7))&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    81|MIDI_C_LSB, rot_y&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    81, (64+(rot_y>>7))&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    82|MIDI_C_LSB, rot_z&0x7F);
                    midi_usb_MidiSend3(1, MIDI_CONTROL_CHANGE,
                                    82, (64+(rot_z>>7))&0x7F);
                } else {
                    // for binary protocol the max is +/-8g, for MIDI max 2g feels better
                    // -(1<<3) for rounding correctly
                    int d; // calculate direction for hysteresis
                    // steps of 32 (acc) and 16 (abs): round to the nearest, a quarter step of
                    // hysteresis (the rounding added a full step, a +1/2 step bias, and the
                    // direction compared at twice the scale: no hysteresis, SC-21)
                    d = ((((last_acc_x-64)<<5) > acc_x)<<4)-(1<<3);
                    acc_x = __USAT(64+((acc_x+(1<<4)+d)>>5), 7)&0x7F;
                    d = ((((last_acc_y-64)<<5) > acc_y)<<4)-(1<<3);
                    acc_y = __USAT(64+((acc_y+(1<<4)+d)>>5), 7)&0x7F;
                    d = ((((last_acc_z-64)<<5) > acc_z)<<4)-(1<<3);
                    acc_z = __USAT(64+((acc_z+(1<<4)+d)>>5), 7)&0x7F;
                    d = (((last_acc_abs<<4) > acc_abs)<<3)-(1<<2);
                    acc_abs = __USAT((acc_abs+(1<<3)+d)>>4, 7)&0x7F;
                    d = ((((last_rot_x-64)<<6) > rot_x)<<5)-(1<<4);
                    rot_x = __USAT(64+((rot_x+(1<<5)+d)>>6), 7)&0x7F;
                    d = ((((last_rot_y-64)<<6) > rot_y)<<5)-(1<<4);
                    rot_y = __USAT(64+((rot_y+(1<<5)+d)>>6), 7)&0x7F;
                    d = ((((last_rot_z-64)<<6) > rot_z)<<5)-(1<<4);
                    rot_z = __USAT(64+((rot_z+(1<<5)+d)>>6), 7)&0x7F;
                    if (acc_x != last_acc_x) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           16, acc_x);
                        sent_at[0][acc_x] = chVTGetSystemTime() + 1;
                        last_acc_x = acc_x;
                    }
                    if (acc_y != last_acc_y) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           17, acc_y);
                        sent_at[1][acc_y] = chVTGetSystemTime() + 1;
                        last_acc_y = acc_y;
                    }
                    if (acc_z != last_acc_z) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           18, acc_z);
                        sent_at[2][acc_z] = chVTGetSystemTime() + 1;
                        last_acc_z = acc_z;
                    }
                    if (acc_abs != last_acc_abs) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           19, acc_abs);
                        last_acc_abs = acc_abs;
                    }
                    if (rot_x != last_rot_x) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           80, rot_x);
                        last_rot_x = rot_x;
                    }
                    if (rot_y != last_rot_y) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           81, rot_y);
                        last_rot_y = rot_y;
                    }
                    if (rot_z != last_rot_z) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           82, rot_z);
                        last_rot_z = rot_z;
                    }
                }
            }
#endif
        }
};

class Button {
    public:
        int coord0;
        int coord1;
        float note;
        int midinote;
        int midinote_base;
        float start_note_offset;
        float tuning_note_offset = 0.0f;
        float pres;
        float vpres;
        int last_pres = INT32_MAX;
        int last_velo = INT32_MAX;
        int last_rvelo = INT32_MAX;
        int last_tilt = INT32_MAX;
        int last_bend = INT32_MAX;
        int last_pitchbend = INT32_MAX;
        float but_x;
        float but_y;
        float vol0;
        float vol = 0.0;
        button_state_t state = STATE_OFF;
        int voice = -1;        // MIDI voice: the MPE channel slot (index into voices[])
        uint8_t midi_channel = 0;  // the channel of its MIDI note-on (end_all_notes)
        bool stolen = false;  // its channel was taken while it was held (get_voice), until released

        Button() {
        }

        // the last values sent are per key; a note on a channel (its own, or one handed over)
        // sends its own pitch bend, pressure, x and y, not the channel's previous note's (SC-07)
        void new_channel_state(void) {
            last_pres = last_tilt = last_bend = last_pitchbend = INT32_MAX;
        }

        void message(float* msg) {
            pres = msg[0];
            vpres = msg[1];
            but_x = msg[2];
            but_y = msg[3];

            // allow vol below zero to take over oldest voice
            if (pres <= 0.0) {
                vol0 = -1.0;
                stolen = false;
            } else
                vol0 = pres + vpres;
            if (vol < vol0)
                vol = vol0;
        }
};

// the voice allocator's view of the buttons (voice_allocator.h's host): held, and the level floor
struct SynthVoiceHost {
    const Button* buttons;
    bool held(int b) const { return buttons[b].state == STATE_ON; }
    float floor(int b) const { return buttons[b].vol0; }
};

class Instrument {
    public:
        Button buttons[BUTTONCOUNT + MIDI_IN_NOTES];  // the keys, then the notes from MIDI in
        int voices[MAX_VOICECOUNT];
        // the internal synth's voices: which key (or MIDI in note) plays each (voice_allocator.h)
        striso::voices::VoiceAllocator<BUTTONCOUNT + MIDI_IN_NOTES, VOICECOUNT> synth_alloc;
        int portamento_buttons[MAX_PORTAMENTO_BUTTONS];
        float notegen0 = 12.00;
        float notegen1 = 7.00;
        int tuning_color = 0x00aa00;
        int cur_tuning = 0;
        float tuning_note_offset = 0;
        float note_offset = 0;
        float start_note_offset = 62;
        float min_note_offset = 32;
        float max_note_offset = 92;
        int altmode = 0;
        int portamento = 0;
        int transposemode = 0;
        int flipdir = 1;
        int last_button = 0;
        int master_button = -1;
        int transpose_button = -1;
        int transpose_button2 = -1;
        synth_interface_t* synth_interface;
        int voicecount = VOICECOUNT;
        int midi_channel_offset = 1;
        float midi_bend_range = 48.0;
        float bend_sensitivity = 1.0f;
        float y_sensitivity = 1.0f;
        float pres_sensitivity = 1.0f;
        float velo_sensitivity = 1.0f;
        int midi_velo_offset = 0;
        // the channel layout the notes were started in (channels_changed)
        midi_mode_t layout_mode = MIDI_MODE_MPE;
        int layout_offset = 1;
        int layout_voicecount = VOICECOUNT;

        Instrument(const int* c0, const int* c1, int n_buttons, synth_interface_t* si) {
            int n;
            for (n = 0; n < n_buttons; n++) {
                buttons[n].coord0 = c0[n];
                buttons[n].coord1 = c1[n];
                // calculate note number
                buttons[n].note = start_note_offset + buttons[n].tuning_note_offset +
                                  notegen0 * buttons[n].coord0 +
                                  notegen1 * buttons[n].coord1;
                buttons[n].midinote_base = (int)(notegen0 * buttons[n].coord0 +
                                                 notegen1 * buttons[n].coord1 + 0.5 + 100) - 100; // careful to keep rounded value above zero
                buttons[n].midinote = buttons[n].midinote_base + (int)(start_note_offset + 0.5);
            }
            for (n = 0; n < MAX_VOICECOUNT; n++) {
                voices[n] = -1;
            }
            for (n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                portamento_buttons[n] = -1;
            }
            synth_interface = si;
            set_volume(volume_linear);
            midi_in.set_id_base(BUTTONCOUNT);  // a MIDI note's id: its Button after the keys
        }

        /*
         * The internal synth is played in MIDI 2.0: the Striso UMP profile (striso_dsp/ump.h).
         * The keys (update_synth_voice) and MIDI in (striso_dsp/midi_in.h: MIDI 1.0, MPE, the
         * up-converter shared with the SDK's hosts) send it; one decoder (synth_in) gives the
         * engines their controls. A note's id is its Button: a key 0 .. BUTTONCOUNT-1, a MIDI in
         * note BUTTONCOUNT + its slot. MIDI notes take an internal voice from the same pool as the
         * keys (get_synth_voice), so keys and MIDI notes steal from each other by the same rule;
         * nothing of them goes out as MIDI. The Instrument is the decoder's host (the note_*
         * calls below) and midi_in's (ump, echoed, bend_range); the instrument's bend, the bend
         * range (RPN 0), the zone (MCM) and the other CCs are midi_in_message's. Sustain (CC64)
         * is the pedal, as before.
         */
        striso::midi::NoteInput midi_in;
        striso::ump::Decoder<BUTTONCOUNT + MIDI_IN_NOTES> synth_in;
        striso::ump::StandardOut<BUTTONCOUNT> ump_out;  // MIDI mode midi2: the keys as MIDI 2.0 (ump_out.h)
        bool key_out[BUTTONCOUNT] = {};                   // a key's Note On went out on MIDI 2.0
        bool key_sounding[BUTTONCOUNT] = {};  // a key's Note On went out (its Note Off not yet)

        // the decoder's host: the touch factors are the keys' sensitivities, a MIDI note's 1
        striso::ump::Touch touch(int id) const {
            return id < BUTTONCOUNT ? striso::ump::Touch{pres_sensitivity, velo_sensitivity, y_sensitivity}
                                    : striso::ump::Touch{1.0f, 1.0f, 1.0f};
        }

        // a MIDI note starts: its Button, its level, a voice (a key's voice is the key logic's)
        void note_started(int id, const striso::ump::NoteSignals& n) {
            if (id < BUTTONCOUNT) return;
            Button& b = buttons[id];
            b.state = STATE_ON;
            b.vpres = n.vpres;
            b.pres = n.pres;
            b.but_x = n.but_x;
            synth_alloc.set_level(id, b.pres + b.vpres);  // a new note: no old estimate
            get_synth_voice(id);  // (-1: every voice plays a louder held note)
        }

        void note_changed(int id, const striso::ump::NoteSignals& n) {
            if (id >= BUTTONCOUNT) {
                Button& b = buttons[id];
                b.state = n.on ? STATE_ON : STATE_OFF;
                b.note = n.note;
                b.pres = n.pres;
                b.vpres = n.vpres;
                b.but_x = n.but_x;
                b.but_y = n.but_y;
                b.vol0 = b.state == STATE_ON ? b.pres + b.vpres : -1.0f;
                synth_alloc.raise_level(id, b.vol0);
            }
            write_synth_voice(id, n);
        }

        void note_silenced(int id) { silence_synth_voice(id); }

        void note_dropped(int id) {
            silence_synth_voice(id);
            buttons[id].state = STATE_OFF;
            buttons[id].vol0 = -1.0f;
        }

        // midi_in's host: its packets go to the decoder (flushed after each message)
        void ump(const striso::ump::Packet& p) { synth_in.packet(p); }

        bool echoed(int channel) const { return note_out_recently(channel); }  // (SC-16)

        float bend_range() const { return midi_bend_range; }

        // the internal synth's pedal control; without an internal synth (synth_interface NULL) a
        // stand-in, so the pedal, decay and CC64 code doesn't dereference NULL (SC-38)
        float* synth_pedal(void) {
            static float none = 3.0f / 16;
            return synth_interface ? synth_interface->pedal : &none;
        }

        // the channel of the instrument-wide controls (sustain/decay CC64, volume CC7): channel 1
        // in MPE (the master channel), else the mode's one channel (SC-18)
        uint8_t control_channel(void) const {
            return config.midi_mode == MIDI_MODE_MPE ? 0 : midi_channel_offset & 0x0f;
        }

        // the MIDI channel of voice (MPE channel slot) n: offset + n, kept in 0..15 (channels_changed)
        uint8_t voice_channel(int n) const {
            return (midi_channel_offset + n) & 0x0f;
        }

        // After a change of the MIDI mode, the channels or the channel count. Keeps the channels
        // offset .. offset + voicecount - 1 within the 16 MIDI channels: a channel number above 15
        // runs into the status nibble (note-off as note-on, B4). If the layout changed, the notes
        // of the old one end (SC-02, SC-31).
        void channels_changed(void) {
            midi_channel_offset = clamp(midi_channel_offset, 0, 15);
            voicecount = clamp(voicecount, 1, min(MAX_VOICECOUNT, 16 - midi_channel_offset));
            if (config.midi_mode != layout_mode || midi_channel_offset != layout_offset
                || voicecount != layout_voicecount) {
                end_all_notes();
                layout_mode = config.midi_mode;
                layout_offset = midi_channel_offset;
                layout_voicecount = voicecount;
            }
        }

        // End every note: a MIDI note-off on the channel of its note-on, the internal voices of
        // the held keys to pres 0; and forget the key, channel and portamento state. Keys still
        // held start a new note with their next message. The settings keys (STATE_ALT) stay.
        void end_all_notes(void) {
            for (int b = 0; b < BUTTONCOUNT; b++) {
                Button& x = buttons[b];
                if (x.state == STATE_OFF || x.state == STATE_ALT) continue;
#ifdef USE_MIDI_OUT
                if (x.state == STATE_ON) {
                    MidiSend3(MIDI_NOTE_OFF | x.midi_channel, x.midinote, 0);
                }
#endif
                silence_key(b);
                x.state = STATE_OFF;
            }
            for (int n = 0; n < MAX_VOICECOUNT; n++) {
                voices[n] = -1;
            }
            for (int n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                portamento_buttons[n] = -1;
            }
            master_button = -1;
            transpose_button = -1;
            transpose_button2 = -1;
        }

        // the panic key: every note and sound off, MIDI and internal
        void panic(void) {
            end_all_notes();
            midi_in.all_off(*this);
            synth_in.flush(*this);
#ifdef USE_MIDI_OUT
            // the MPE master channel, and every channel of the mode
            int first = config.midi_mode == MIDI_MODE_MPE && midi_channel_offset >= 1 ? -1 : 0;
            int count = config.midi_mode == MIDI_MODE_POLY ? 1 : voicecount;
            for (int n = first; n < count; n++) {
                MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(n), MIDI_C_ALL_SOUND_OFF, 0);
                MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(n), MIDI_C_ALL_NOTES_OFF, 0);
            }
#endif
#ifdef USE_INTERNAL_SYNTH
            for (int v = 0; v < VOICECOUNT && synth_interface; v++) {
                *(synth_interface->pres[v])  = 0.0f;
                *(synth_interface->vpres[v]) = 0.0f;
            }
#endif
        }

        void set_portamento(int p) {
            // in mono mode portamento should stay on
            if (config.midi_mode != MIDI_MODE_MPE || transposemode) {
                return;
            }
            if (p) {
                if (!portamento) {
                    portamento = 1;
                    if (buttons[last_button].state == STATE_ON) {
                        master_button = last_button;
                    }
                    // TODO: else check if only one button is pressed
                }
            } else if (portamento) {
                portamento = 0;
            }
        }

        void set_altmode(int a) {
            if (a) {
                altmode |= 1;
            } else {
                altmode &= 2;
            }
        }

        void set_free_transpose_mode(int p) {
            if (portamento) {
                return;
            }
            if (p) {
                if (!transposemode) {
                    transposemode = 1;
                    if (buttons[last_button].state == STATE_ON) {
                        transpose_button = last_button;
                    } else {
                        transpose_button = -1;
                    }
                    transpose_button2 = -1;
                }
            } else if (transposemode) {
                transposemode = 0;
                transpose_button = -1;
                transpose_button2 = -1;
            }
        }

        int change_note_offset(float offset) {
            return set_note_offset(start_note_offset + offset);
        }

        int set_note_offset(float offset) {
            if (offset >= min_note_offset && offset <= max_note_offset) {
                start_note_offset = offset;
                update_leds();
                return 0;
            }
            led_rgb3(255,0,0);
            return 1;
        }

        void set_notegen1(float g) {
            // unset cur_tuning so the tuning will be reset on tuning switch
            cur_tuning = -1;
            // keep generator within syntonic continuum range
            notegen1 = clamp(g, 6.85714285714286f, 7.2f);

            // base color depending on tuning system
            tuning_color = led_view::generator_color(notegen1);

            update_leds();
        }

        void reset_note_offsets(void) {
            for (int n = 0; n < 61; n++) {
                int but = button_number_map[n];
                buttons[but].tuning_note_offset = 0.0f;
            }
        }

        void set_note_offsets(float* offsets, int n_buttons) {
            if (n_buttons == 61) {
                // all 61 offsets are given
                for (int n = 0; n < 61; n++) {
                    int but = button_number_map[n];
                    buttons[but].tuning_note_offset = offsets[n] / 100;
                }
            }
            else if (n_buttons == 17) {
                // one repeating octave starting with C
                for (int n = 0; n < 61; n++) {
                    int but = button_number_map[n];
                    buttons[but].tuning_note_offset = offsets[(n + 7) % 17] / 100;
                }
            }
        }

        // tuning n (PresetLoader::load_tuning, after the class)
        void load_tuning(int n);

        /* Rotate the layout 180 degrees */
        void flip(void) {
            flipdir = -flipdir;
            for (int n = 0; n < BUTTONCOUNT; n++) {
                buttons[n].coord0 = -buttons[n].coord0;
                buttons[n].coord1 = -buttons[n].coord1;
                // invert midi note number
                buttons[n].midinote_base = -buttons[n].midinote_base;
            }
            // each key now plays the mirrored key's note: its tuning offset goes with the note (SC-12)
            for (int n = 0; n < 30; n++) {
                Button& a = buttons[button_number_map[n]];
                Button& b = buttons[button_number_map[60 - n]];
                float t = a.tuning_note_offset;
                a.tuning_note_offset = b.tuning_note_offset;
                b.tuning_note_offset = t;
            }
        }

        void button_message(int but, float* msg) {
            static float old_angle = -1000.0f;
            static bool nudged = false;
            static systime_t next_knobchange;
            // process button message and send osc messages
            buttons[but].message(msg);
            synth_alloc.raise_level(but, buttons[but].vol0);

            // handle alternative functions of note buttons
            if ((altmode == 1 && buttons[but].state == STATE_OFF)
                || (buttons[but].state == STATE_ALT)) {
                // only handle on new press
                if (buttons[but].state == STATE_OFF && buttons[but].pres > 0.05) {
                    buttons[but].state = STATE_ALT;
                    altmode |= 2;
                    config_but(but, 0, 0); // show current setting
                } else if (buttons[but].pres == 0.0) {
                    buttons[but].state = STATE_OFF;
                    altmode &= 1;
                    old_angle = -1000.0f;
                    nudged = false;
                    if (!altmode) update_leds();
                }
                if (buttons[but].state != STATE_ALT) {
                    // a touch too light to count as a press (its x/y are noise) or a release:
                    // no knob or nudge on a key whose setting isn't shown (SC-09)
                    return;
                }
                // handle knob mode
                float a = pow2(buttons[but].but_x) + pow2(buttons[but].but_y);
                if (a > pow2(0.5f)) {
                    float angle = atan2f(buttons[but].but_x, buttons[but].but_y)*(8/3.1415926536) + 8.0f;
                    if (old_angle == -1000.0f) {
                        // a time from an earlier press may be any age (time_after only orders times
                        // less than 12 days apart)
                        next_knobchange = chVTGetSystemTime();
                        // nudge up or down
                        if (buttons[but].but_x < 0.3 && buttons[but].but_x > -0.3) {
                            if (buttons[but].but_y > 0) {
                                config_but(but, 1, 1.0f);
                            } else {
                                config_but(but, 1, -1.0f);
                            }
                            nudged = true;
                            next_knobchange = chVTGetSystemTime() + TIME_MS2I(500);
                        }
                        old_angle = angle;
                    } else {
                        float adjust = angle - old_angle;
                        if (adjust > 8) adjust -= 16;
                        else if (adjust < -8) adjust += 16;
                        if (nudged) {
                            if (fabsf(adjust) > 4.0f) {
                                nudged = false;
                                old_angle = angle;
                            } else {
                                if (time_after(chVTGetSystemTime(), next_knobchange)) {
                                    // nudge up or down
                                    if (buttons[but].but_y > 0) {
                                        config_but(but, 1, 1.0f);
                                    } else {
                                        config_but(but, 1, -1.0f);
                                    }
                                    // faster the further up or down; y can be exactly 0 (rolled to the side): at most 10 s
                                    float y2 = pow2(buttons[but].but_y);
                                    next_knobchange = chVTGetSystemTime() + TIME_MS2I(y2 > 0.005f ? 200 / (4*y2) : 10000.0f);
                                }
                            }
                        } else {
                            if (time_after(chVTGetSystemTime(), next_knobchange)) {
                                adjust = config_but(but, 2, adjust);
                                old_angle = angle - clamp(adjust, -4, 4);
                                next_knobchange = chVTGetSystemTime() + TIME_MS2I(20);
                            }
                        }
                    }
                } else if (a < pow2(0.2f)) {
                    old_angle = -1000.0f;
                    nudged = false;
                }
                return;
            }

            if (config.midi_mode == MIDI_MODE_POLY) {
                // in single channel poly mode just send out the notes on a single channel,
                // skip the whole channel assignment stuff.
#ifdef USE_MIDI_OUT
                if (buttons[but].pres > 0.0) {
                    // Note on detection
                    if (buttons[but].state == STATE_OFF) {
                        buttons[but].state = STATE_ON;
                        buttons[but].midinote = buttons[but].midinote_base + (int)(start_note_offset + 0.5);
                        buttons[but].midinote = clamp(buttons[but].midinote, 0, 127);
                        buttons[but].start_note_offset = start_note_offset;
                        get_synth_voice(but);
                        // multiply velo by 2 to cover full midi range on note on
                        int velo = to_int_clamped(midi_velo_offset + buttons[but].vpres * velo_sensitivity * 128 * 2, 1, 127);
                        buttons[but].midi_channel = midi_channel_offset;
                        MidiSend3(MIDI_NOTE_ON | midi_channel_offset,
                                        buttons[but].midinote, velo);

                        master_button = but;
                    }
                    if (master_button == -1) {
                        master_button = but;
                    }
                    // the internal synth plays every key with its own expression, as in MPE mode
                    buttons[but].note = buttons[but].start_note_offset + buttons[but].tuning_note_offset +
                                        notegen0 * buttons[but].coord0 +
                                        notegen1 * buttons[but].coord1
                                        + note_offset;
                    update_synth_voice(but);

                    if (config.midi_pres == CFG_POLY_PRESSURE) {
                        float presf = buttons[but].pres * pres_sensitivity;
                        float d; // calculate direction for hysteresis
                        d = (buttons[but].last_pres > presf * 127) * 0.5 - 0.25; // (last_pres is 0..127)
                        int pres = to_int_clamped(presf * 127 + 0.5 + d, 0, 127);

                        if (pres != buttons[but].last_pres) {
                            MidiSend3(MIDI_POLY_PRESSURE | midi_channel_offset,
                                            buttons[but].midinote, pres);
                            buttons[but].last_pres = pres;
                        }
                    }

                    if (but == master_button) {
                        // reduce pres, bend and tilt of all pressed buttons to single values
                        float presf = 0.0f;
                        float x = 0.0f;
                        float xw = 0.0f;
                        float y = 0.0f;
                        float yw = 0.0f;
                        for (int i=0; i<BUTTONCOUNT; i++) {
                            if (buttons[i].state == STATE_ON) {
                                if (buttons[i].pres > presf) presf = buttons[i].pres;
                                float w = pow2(buttons[i].but_x);
                                x += w * buttons[i].but_x;
                                xw += w;
                                w = pow2(buttons[i].but_y);
                                y += w * buttons[i].but_y;
                                yw += w;
                            }
                        }
                        if (xw > 0.0000001f) x /= xw;
                        if (yw > 0.0000001f) y /= yw;

                        float d; // calculate direction for hysteresis

                        if (config.midi_pres != CFG_POLY_PRESSURE) {
                            d = (buttons[0].last_pres > presf * 127) * 0.5 - 0.25; // (last_pres is 0..127)
                            int pres = to_int_clamped(presf * 127 + 0.5 + d, 0, 127);
                            if (pres != buttons[0].last_pres) {
                                if (config.midi_pres == CFG_CHANNEL_PRESSURE) {
                                    MidiSend2(MIDI_CHANNEL_PRESSURE | midi_channel_offset,
                                                    pres);
                                } else if (config.midi_pres < 120) {
                                    MidiSend3(MIDI_CONTROL_CHANGE | midi_channel_offset,
                                                    config.midi_pres, pres);
                                }
                                buttons[0].last_pres = pres;
                            }
                        }
                        if (config.midi_x == CFG_PITCH_BEND) {
                            // bend_sensitivity semitones at full x, as in MPE, of Normal mode's
                            // bend range of 2 (midi_config); it bent twice as far as MPE (SC-08)
                            x = pow3(x) * bend_sensitivity * 0.5f;
                            d = (buttons[0].last_bend > (0x2000 + x * 0x2000)) * 0.5 - 0.25;
                            int bend = to_int_clamped(0x2000 + x * 0x2000 + 0.5 + d, 0, 0x3fff);
                            if (bend != buttons[0].last_bend) {
                                MidiSend3(MIDI_PITCH_BEND | midi_channel_offset,
                                          bend & 0x7f, (bend >> 7) & 0x7f);
                                buttons[0].last_bend = bend;
                            }
                        } else if (config.midi_x < 120) {
                            d = (buttons[0].last_bend > (64 + x * 64)) * 0.5 - 0.25;
                            int bend = to_int_clamped(64 + x * 64 + 0.5 + d, 0, 127);
                            if (bend != buttons[0].last_bend) {
                                MidiSend3(MIDI_CONTROL_CHANGE | midi_channel_offset,
                                                config.midi_x, bend);
                                buttons[0].last_bend = bend;
                            }
                        }
                        y = y * y_sensitivity;
                        d = (buttons[0].last_tilt > (64 + y * 64)) * 0.5 - 0.25;
                        int tilt = to_int_clamped(64 + y * 64 + 0.5 + d, 0, 127);
                        if (tilt != buttons[0].last_tilt) {
                            if (config.midi_y < 120 && y_sensitivity != 0.0f) {
                                MidiSend3(MIDI_CONTROL_CHANGE | midi_channel_offset,
                                                config.midi_y, tilt);
                            }
                            buttons[0].last_tilt = tilt;
                        }
                    }

                } else { // Note off
                    if (buttons[but].state == STATE_ON) {
                        update_synth_voice(but);  // pres 0, the release velocity
                    }
                    buttons[but].state = STATE_OFF;
                    int velo = to_int_clamped(0 - buttons[but].vpres * velo_sensitivity * 128 * 2, 0, 127);
                    MidiSend3(MIDI_NOTE_OFF | midi_channel_offset,
                                    buttons[but].midinote, velo);
                    if (but == master_button) {
                        master_button = -1;
                    }
                }
                return;
#endif
            }

            // Note on detection
            if (buttons[but].state == STATE_OFF && buttons[but].pres > 0.0) {
                // calculate midinote only at note on
                if (config.midinote_mode == MIDINOTE_MODE_DEFAULT) {
                    buttons[but].midinote = buttons[but].midinote_base + (int)(start_note_offset + 0.5);
                } else if (config.midinote_mode == MIDINOTE_MODE_TUNING) {
                    buttons[but].midinote = to_int_clamped(notegen0 * buttons[but].coord0 +
                                                  notegen1 * buttons[but].coord1 +
                                                  start_note_offset + buttons[but].tuning_note_offset +
                                                  note_offset + 0.5, 0, 127);
                } else if (config.midinote_mode == MIDINOTE_MODE_BUTTON) {
                    buttons[but].midinote = 17 * buttons[but].coord0 + 10 * buttons[but].coord1 + 30;
                }
                // a MIDI note number is 0..127 (a byte with bit 7 set is a status byte); the pitch
                // bend carries what's beyond (SC-11)
                buttons[but].midinote = clamp(buttons[but].midinote, 0, 127);
                buttons[but].start_note_offset = start_note_offset;
                // calculate note pitch
                buttons[but].note = buttons[but].start_note_offset + buttons[but].tuning_note_offset +
                                    notegen0 * buttons[but].coord0 +
                                    notegen1 * buttons[but].coord1
                                    + note_offset;

                if (portamento) {
                    if (master_button == -1) {
                        if (get_voice(but) >= 0) {
                            master_button = but;
                        }
                    } else {
                        // add button to portamento button
                        for (int n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                            if (portamento_buttons[n] == -1) {
                                portamento_buttons[n] = but;
                                buttons[but].state = STATE_PORTAMENTO;
                                break;
                            }
                        }
                    }
                } else if (transposemode) {
                    if (transpose_button == -1) {
                        if (get_voice(but) >= 0) {
                            transpose_button = but;
                        }
                    } else if (buttons[transpose_button].state == STATE_ON
                               && buttons[but].pres > 0.1) {
                        buttons[but].state = STATE_TRANSPOSE;
                        transpose_button2 = but;
                        transposemode = 0;
                    }
                } else {
                    get_voice(but);
                }
            }

            if (buttons[but].state) {
                // calculate note pitch
                buttons[but].note = buttons[but].start_note_offset + buttons[but].tuning_note_offset +
                                    notegen0 * buttons[but].coord0 +
                                    notegen1 * buttons[but].coord1
                                    + note_offset;

                if (but == master_button) {
                    // calculate average of portamento buttons
                    float temp_pres = buttons[but].pres; // save pres for note off detection

                    float pres = buttons[but].pres;
                    float sw = pres*pres;
                    float vpres = buttons[but].vpres;
                    float note  = sw * buttons[but].note;
                    float but_x = sw * buttons[but].but_x;
                    float but_y = sw * buttons[but].but_y;
                    for (int n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                        int b = portamento_buttons[n];
                        if (b >= 0) {
                            float w = buttons[b].pres;
                            w = w*w;
                            sw += w;
                            pres  += buttons[b].pres;
                            vpres += buttons[b].vpres;
                            note  += w * buttons[b].note;
                            but_x += w * buttons[b].but_x;
                            but_y += w * buttons[b].but_y;
                        }
                    }
                    buttons[but].pres = min(pres, 1.0f);
                    buttons[but].vpres = clamp(vpres, -1.0f, 1.0f);

                    if (sw > 0.0f) {
                        buttons[but].note = note / sw;
                        buttons[but].but_x = but_x / sw;
                        buttons[but].but_y = but_y / sw;
                    }

                    update_voice(but);
                    buttons[but].pres = temp_pres;
                }
                else if (but == transpose_button && transpose_button2 >= 0) {
                    // calculate combined transpose buttons
                    float temp_pres = buttons[but].pres; // save pres for note off detection

                    float sw = buttons[but].pres + buttons[transpose_button2].pres;
                    if (sw > 0.0f) {
                        buttons[but].but_x = (buttons[but].pres * buttons[but].but_x +
                                            buttons[transpose_button2].pres * buttons[transpose_button2].but_x)
                                            / sw;
                        buttons[but].but_y = (buttons[but].pres * buttons[but].but_y +
                                            buttons[transpose_button2].pres * buttons[transpose_button2].but_y)
                                            / sw;
                    }
                    buttons[but].pres = max(buttons[but].pres, buttons[transpose_button2].pres);
                    buttons[but].vpres = buttons[but].vpres + buttons[transpose_button2].vpres;

                    update_voice(but);
                    buttons[but].pres = temp_pres;
                }
                else if (buttons[but].state == STATE_ON) {
                    // send synth parameters
                    update_voice(but);
                }

                // Note off detection
                if (buttons[but].pres <= 0) {
                    if (buttons[but].state == STATE_PORTAMENTO) {
                        int count = 0;
                        for (int n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                            if (portamento_buttons[n] == but) {
                                portamento_buttons[n] = -1;
                            }
                            count += portamento_buttons[n] >= 0;
                        }
                        buttons[but].state = STATE_OFF;
                        if (!portamento && count == 0) {
                            // if no portamento buttons left turn off portamento
                            master_button = -1;
                        }
                    } else if (but == master_button) {
                        // find other portamento button to take over portamento voice
                        for (int n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                            if (portamento_buttons[n] >= 0) {
                                buttons[portamento_buttons[n]].midinote = buttons[but].midinote;
                                buttons[portamento_buttons[n]].voice = buttons[but].voice;
                                buttons[portamento_buttons[n]].midi_channel = buttons[but].midi_channel;
                                buttons[portamento_buttons[n]].new_channel_state();
                                master_button = portamento_buttons[n];
                                voices[buttons[master_button].voice] = master_button;
                                hand_over_synth_voice(but, master_button);
                                portamento_buttons[n] = -1;
                                buttons[master_button].state = STATE_ON;
                                buttons[but].state = STATE_OFF;
                                break;
                            }
                        }
                        // if no other portamento button is found turn off portamento
                        if (buttons[but].state) {
                            master_button = -1;
                        }
                    } else if (but == transpose_button) {
                        if (transpose_button2 >= 0
                            && buttons[transpose_button2].state == STATE_TRANSPOSE) {
                            // do free transpose
                            float shift = buttons[transpose_button].note - buttons[transpose_button2].note;
                            if (change_note_offset(shift) == 0) {
                                buttons[transpose_button2].start_note_offset = start_note_offset;
                            } else {
                                // beyond the transposition range (the led shows red): it stays, and
                                // the second key alone continues the first's pitch (SC-22)
                                buttons[transpose_button2].start_note_offset += shift;
                            }
                            buttons[transpose_button2].midinote = buttons[transpose_button].midinote;
                            buttons[transpose_button2].voice = buttons[transpose_button].voice;
                            buttons[transpose_button2].midi_channel = buttons[transpose_button].midi_channel;
                            buttons[transpose_button2].new_channel_state();
                            voices[buttons[transpose_button2].voice] = transpose_button2;
                            hand_over_synth_voice(transpose_button, transpose_button2);
                            buttons[transpose_button2].state = STATE_ON;
                            buttons[but].state = STATE_OFF;
                        }
                        transpose_button = -1;
                        transpose_button2 = -1;
                    } else if (buttons[but].state == STATE_TRANSPOSE) {
                        buttons[but].state = STATE_OFF;
                        transpose_button = -1;
                        transpose_button2 = -1;
                    }
                    if (but == last_button) {
                        // if last pressed button is released check if other button can take its place
                        for (int n = 0; n < voicecount; n++) {
                            if (voices[n] >= 0 && buttons[voices[n]].state == STATE_ON) {
                                last_button = voices[n];
                                break;
                            }
                        }
                    }

                    release_voice(but);
                };
            }
        }

        /*
         * Internal synth voices. The internal synth has VOICECOUNT voices, allocated apart from the
         * MIDI channels and the same way in every MIDI mode, so every note sounds whatever the MPE
         * channel count (SC-01), and Normal mode plays the internal synth too, each key with its
         * own expression (SC-10). Mono mode keeps its one voice: its MIDI voice (0).
         */
        // the internal voice for a new note of but (voice_allocator.h's rule); -1 if none
        int get_synth_voice(int but) {
#ifdef USE_INTERNAL_SYNTH
            if (config.midi_mode == MIDI_MODE_MONO && but < BUTTONCOUNT) {
                // Mono mode keeps its one voice: its MIDI voice (0); a MIDI in note takes another
                const int voice = buttons[but].voice >= 0 && buttons[but].voice < VOICECOUNT ? buttons[but].voice : -1;
                if (voice >= 0) synth_alloc.take(but, voice);
                return voice;
            }
            return synth_alloc.start(SynthVoiceHost{buttons}, but, config.midi_mode == MIDI_MODE_MONO ? 1 : 0);
#else
            (void)but;
            return -1;
#endif
        }

        // the note of but goes on with key to (portamento and free transpose hand-overs)
        void hand_over_synth_voice(int but, int to) { synth_alloc.hand_over(but, to); }

        // a key's controls in the Striso UMP profile: pitch (with the tilt's bend), pres, vpres,
        // x, y, and its Note On / Off as its state changes; through the decoder to its internal
        // voice (if it still has one), and in MIDI mode midi2 to the MIDI 2.0 out
        void update_synth_voice(int but) {
            const Button& b = buttons[but];
            float pb = 0.0f;
            if (config.mpe_x == CFG_PITCH_BEND) {
                pb = bend_sensitivity * pow3(b.but_x);
            }
            const bool on = b.state == STATE_ON;
            striso::ump::Packet p[6] = {
                striso::ump::pitch_packet(but, b.note + pb),
                striso::ump::pres_packet(but, b.pres),
                striso::ump::signed_packet(but, striso::ump::ctl_vpres, b.vpres),
                striso::ump::signed_packet(but, striso::ump::ctl_x, b.but_x),
                striso::ump::signed_packet(but, striso::ump::ctl_y, b.but_y),
                {0, 0}};
            int n = 5;
            if (on != key_sounding[but]) {
                p[n++] = on ? striso::ump::note_on_packet(but, b.vpres, false) : striso::ump::note_off_packet(but);
                key_sounding[but] = on;
            }
#ifdef USE_INTERNAL_SYNTH
            for (int k = 0; k < n; k++) synth_in.packet(p[k]);
            synth_in.flush(*this);
#endif
            if (config.midi2) {
                for (int k = 0; k < 5; k++) ump_out.packet(p[k]);
                if (on != key_out[but]) {
                    ump_out.packet(on ? striso::ump::note_on_packet(but, b.vpres, false) : striso::ump::note_off_packet(but));
                    key_out[but] = on;
                }
                ump_out.flush(*this);
            }
        }

        // MIDI 2.0 out: a key whose state changed after its last controls (a release: they went
        // out while it was on; portamento, transpose, the panic) gets its Note On / Off after its
        // key message. (The internal synth's decoder doesn't need it: a key's voice is the key
        // logic's.)
        // (After midi2 is left, its notes still get their Note Off.)
        void sync_key_states() {
            bool any = false;
            for (int but = 0; but < BUTTONCOUNT; but++) {
                const bool on = buttons[but].state == STATE_ON;
                if (on == key_out[but] || (on && !config.midi2)) continue;
                ump_out.packet(on ? striso::ump::note_on_packet(but, buttons[but].vpres, false) : striso::ump::note_off_packet(but));
                key_out[but] = on;
                any = true;
            }
            if (any) ump_out.flush(*this);
        }

        // ump_out's sink: USB's MIDI 2.0 alt setting (nothing on the MIDI 1.0 one, or on DIN). A
        // full USB queue drops a message (midi_usb_ump_drops), but never a Note Off: that waits
        // (ump_off_w1, per note number) and goes ahead of everything else, at the next message or
        // synth_tick's retry; while one waits, the rest is held back (counted) so a later Note On
        // can't overtake it.
        uint32_t ump_off_w1[128] = {};
        uint32_t ump_off_waiting[4] = {};
        volatile bool ump_offs_waiting = false;
        uint32_t ump_held_back = 0, ump_offs_retried = 0;

        bool ump_offs_flush() {
            if (!ump_offs_waiting) return true;
            for (int n = 0; n < 128; n++) {
                if (!(ump_off_waiting[n >> 5] & (1u << (n & 31)))) continue;
                const uint32_t w[2] = {0x40800000u | (uint32_t)n << 8, ump_off_w1[n]};
                if (!midi_usb_UmpSend(w, 2)) return false;
                ump_off_waiting[n >> 5] &= ~(1u << (n & 31));
            }
            ump_offs_waiting = false;
            return true;
        }

        void ump(uint32_t w0, uint32_t w1) {
            const uint32_t w[2] = {w0, w1};
            const bool flushed = ump_offs_flush();
            if (flushed && midi_usb_UmpSend(w, 2)) return;
            if (!flushed) ump_held_back++;
            if ((w0 & 0xfff00000u) == 0x40800000u) {  // a Note Off (group 1, channel 1): it waits
                const int n = (int)((w0 >> 8) & 0x7f);
                ump_off_w1[n] = w1;
                ump_off_waiting[n >> 5] |= 1u << (n & 31);
                ump_offs_waiting = true;
                ump_offs_retried++;
            }
        }

        int ump_offs_pending() const {
            int n = 0;
            for (const uint32_t m : ump_off_waiting) n += std::popcount(m);
            return n;
        }

        // the decoded controls of note id to its voice (if it still has one)
        void write_synth_voice(int id, const striso::ump::NoteSignals& n) {
#ifdef USE_INTERNAL_SYNTH
            const int voice = synth_alloc.voice(id);
            if (voice < 0 || !synth_interface) {
                return;
            }
            *(synth_interface->note[voice])  = n.note;
            *(synth_interface->pres[voice])  = n.pres;
            *(synth_interface->vpres[voice]) = n.vpres;
            *(synth_interface->but_x[voice]) = n.but_x;
            *(synth_interface->but_y[voice]) = n.but_y;
#else
            (void)id;
            (void)n;
#endif
        }

        // end the internal sound of key but's note (its key is still held): through the decoder
        void silence_key(int but) {
#ifdef USE_INTERNAL_SYNTH
            synth_in.packet(striso::ump::management_packet(but, striso::ump::PNM_SILENCE));
            synth_in.flush(*this);
#else
            (void)but;
#endif
        }

        // the decoder's: the internal sound of note id's voice ends
        void silence_synth_voice(int but) {
#ifdef USE_INTERNAL_SYNTH
            const int voice = synth_alloc.voice(but);
            if (voice >= 0 && synth_interface) {
                *(synth_interface->pres[voice])  = 0.0f;
                *(synth_interface->vpres[voice]) = 0.0f;
            }
#else
            (void)but;
#endif
        }

        void release_voice(int but) {
            if (buttons[but].state == STATE_ON) {
                buttons[but].state = STATE_OFF;

#ifdef USE_MIDI_OUT
                int velo = to_int_clamped(0 - buttons[but].vpres * velo_sensitivity * 128 * 2, 0, 127);
                MidiSend3(MIDI_NOTE_OFF | voice_channel(buttons[but].voice),
                                   buttons[but].midinote, velo);
#endif
            }
        }

        int get_voice(int but) {
            int voice = -1;
            float min_vol = buttons[but].vol;
            if (buttons[but].stolen) {
                // a held key that lost its channel takes one back only from a clearly quieter
                // note: two keys at about the same pressure don't steal from each other on every
                // message (SC-06)
                min_vol = min_vol * 0.9f - 0.05f;
            }
            for (int n = 0; n < voicecount; n++) {
                // check if empty or last used voice is available
                if (voices[n] == -1 || voices[n] == but)
                {
                    voice = n;
                    break;
                }
                // else find voice with minimum approximated volume
                float vol = buttons[voices[n]].vol;
                if (vol < min_vol) {
                    min_vol = vol;
                    voice = n;
                }
            }
            if (voice < 0) {
                // none quieter than the new note: a released note's channel is free all the same,
                // the quietest one (else a new note waits until an old one's estimate decays below
                // it: up to about 0.9 s, SC-03)
                float min_released = 0;
                for (int n = 0; n < voicecount; n++) {
                    const Button& b = buttons[voices[n]];
                    if (b.state != STATE_ON && (voice < 0 || b.vol < min_released)) {
                        min_released = b.vol;
                        voice = n;
                    }
                }
            }
            if (voice >= 0) {
                // take over the voice
                if (voices[voice] >= 0) {
                    if (buttons[voices[voice]].state == STATE_ON) {
                        silence_key(voices[voice]);  // a held note stolen
                        buttons[voices[voice]].stolen = buttons[voices[voice]].pres > 0.0f;
                    }
                    release_voice(voices[voice]);
                }

                buttons[but].state = STATE_ON;
                buttons[but].voice = voice;
                buttons[but].new_channel_state();
                voices[voice] = but;
                last_button = but;
                get_synth_voice(but);

#ifdef USE_MIDI_OUT
                // multiply velo by 2 to cover full midi range on note on
                int velo = to_int_clamped(midi_velo_offset + buttons[but].vpres * velo_sensitivity * 128 * 2, 1, 127);

                // send pitch bend before note on for correct tuning
                // TODO: don't send it on 12et tuning?
                // calculate note pitch
                buttons[but].note = buttons[but].start_note_offset + buttons[but].tuning_note_offset +
                                    notegen0 * buttons[but].coord0 +
                                    notegen1 * buttons[but].coord1
                                    + note_offset;
                int pitchbend = to_int_clamped((buttons[but].note - buttons[but].midinote)
                                * (0x2000 / midi_bend_range) + 0x2000 + 0.5, 0, 0x3fff);
                MidiSend3(MIDI_PITCH_BEND | voice_channel(buttons[but].voice),
                                   pitchbend & 0x7f, (pitchbend >> 7) & 0x7f);
                // don't set last_pichbend to send pitchbend again after note_on for compatibility with some MPE implementations
                // buttons[but].last_pitchbend = pitchbend;

                buttons[but].midi_channel = voice_channel(buttons[but].voice);
                MidiSend3(MIDI_NOTE_ON | voice_channel(buttons[but].voice),
                                   buttons[but].midinote, velo);
                buttons[but].last_velo = velo;
#endif

                return 0;
            }
            return -1;
        }

        void update_voice(int but) {
            float pb = 0.0f;
            if (config.mpe_x == CFG_PITCH_BEND) {
                pb = bend_sensitivity * pow3(buttons[but].but_x);
            }
            float presf = buttons[but].pres * pres_sensitivity;
            float velof = buttons[but].vpres * velo_sensitivity;
            float y = clamp(buttons[but].but_y * y_sensitivity, -1.0f, 1.0f);
            update_synth_voice(but);
            if (buttons[but].voice < 0 || buttons[but].voice >= MAX_VOICECOUNT) {
                return;  // (can't happen: the notes end when the channels change)
            }

#ifdef USE_MIDI_OUT
            float d; // calculate direction for hysteresis
            // TODO: fix hysteresis!
            // TODO: last_... values per midi channel instead of per button
            d = (buttons[but].last_pres > presf * 127) * 0.5 - 0.25; // (last_pres is 0..127)
            int pres = to_int_clamped(presf * 127 + 0.5 + d, 0, 127);

            d = (buttons[but].last_tilt > (64 + y * 64)) * 0.5 - 0.25;
            int tilt = to_int_clamped(64 + y * 64 + 0.5 + d, 0, 127);

            pb = (pb
                + buttons[but].note - buttons[but].midinote)
                * (0x2000 / midi_bend_range) + 0x2000;
            d = (buttons[but].last_pitchbend > pb) * 0.5 - 0.25;
            int pitchbend = to_int_clamped(pb + 0.5 + d, 0, 0x3fff);

            // pitchbend is also used for tuning and glissando
            if (pitchbend != buttons[but].last_pitchbend) {
                MidiSend3(MIDI_PITCH_BEND | voice_channel(buttons[but].voice),
                                   pitchbend & 0x7f, (pitchbend >> 7) & 0x7f);
                buttons[but].last_pitchbend = pitchbend;
            }
            if (pres != buttons[but].last_pres) {
                if (config.mpe_pres == CFG_CHANNEL_PRESSURE) {
                    MidiSend2(MIDI_CHANNEL_PRESSURE | voice_channel(buttons[but].voice),
                                    pres);
                } else if (config.mpe_pres < 120) {
                    MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                    config.mpe_pres, pres);
                }
                buttons[but].last_pres = pres;
            }
            if (config.mpe_x < 120) {
                int bend = to_int_clamped(64.5 + buttons[but].but_x * 64, 0, 127);
                if (bend != buttons[but].last_bend) {
                    MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                    config.mpe_x, bend);
                    buttons[but].last_bend = bend;
                }
            }
            if (tilt != buttons[but].last_tilt) {
                if (config.mpe_y < 120) {
                    MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                    config.mpe_y, tilt);
                }
                buttons[but].last_tilt = tilt;
            }
            if (config.mpe_contvelo < 120) {
                // TODO: hysteresis for continuous velocity
                // TODO: make contvelo CC configurable
                if (velof > 0) {
                    int velo = to_int_clamped(0 + velof * 256, 0, 127);
                    if (velo != buttons[but].last_velo) {
                        MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                        73, velo);
                        buttons[but].last_velo = velo;
                    }
                    if (buttons[but].last_rvelo > 0) {
                        MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                        72, 0);
                        buttons[but].last_rvelo = 0;
                    }
                } else {
                    int rvelo = to_int_clamped(0 - velof * 256, 0, 127);
                    if (rvelo != buttons[but].last_rvelo) {
                        MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                        72, rvelo);
                        buttons[but].last_rvelo = rvelo;
                    }
                    if (buttons[but].last_velo > 0) {
                        MidiSend3(MIDI_CONTROL_CHANGE | voice_channel(buttons[but].voice),
                                        73, 0);
                        buttons[but].last_velo = 0;
                    }
                }
            }
#endif
        }

        void tick(void) {
            // approximated volume
            for (int n = 0; n < voicecount; n++) {
                if (voices[n] >= 0) {
                    buttons[voices[n]].vol *= VOL_TICK_FACT;
                    buttons[voices[n]].vol -= VOL_TICK;
                    if (buttons[voices[n]].vol < buttons[voices[n]].vol0)
                        buttons[voices[n]].vol = buttons[voices[n]].vol0;
                }
            }
            // the same for the keys of the internal synth voices
            synth_alloc.tick(SynthVoiceHost{buttons});
        }
};

// the layout coordinates per button: striso::tuning::coord0/coord1 (tuning.h)
static_assert(BUTTONCOUNT == striso::tuning::BUTTON_COUNT, "one coordinate pair per button");

#ifdef USE_INTERNAL_SYNTH
Instrument dis(striso::tuning::coord0, striso::tuning::coord1, BUTTONCOUNT, &synth_interface);
#else
Instrument dis(striso::tuning::coord0, striso::tuning::coord1, BUTTONCOUNT, NULL);
#endif
MotionSensor motion;

MidiParam pedal(MIDI_C_DAMPER);

void int2float(int *msg, float *fmsg, int n) {
    int c;
    for (c=0; c<n; c++) {
        fmsg[c] = ((float)msg[c])/0x1fff; //8191, maximum of 14bit signed int
    }
}

unsigned int aux_button_map = 0;

/*
 * StcK <id>: press a settings key from the USB console (Striso Live's settings overlay), exactly as
 * pressing key <id> with Settings held does at the initial press: config_but(id, 0, 0), then the
 * leds as releasing Settings leaves them (update_leds). The console only queues the request
 * (synth_settings_key, message ID_INTERNAL): it runs in the thread that handles the keys
 * (synth_message, the control thread), so it never races a real key press.
 * Allowed: the keys flagged kConsole in the settings-key table (settings_key_table, next to
 * config_but; StcX lists them with the flag 'k'): the MIDI modes, the tunings, the settings presets,
 * the voices of the engines in this firmware and panic. Refused: system reset, send cfg, flip, the
 * dials (they need an amount, not a press) and anything else. None of the allowed keys writes flash
 * or reboots.
 */
static bool settings_key_allowed(int id);

static void settings_key_press(int id) {
    if (!settings_key_allowed(id)) return;
    config_but(id, 0, 0);
    update_leds();
}

int synth_settings_key(int id) {
    if (!settings_key_allowed(id)) return 0;
    return messaging::send(Source::internal, IDI_SETTINGS_KEY, id);
}

int synth_message(int size, int* msg) {
    static float note_offset_old;
    float fmsg[7];
    int src = msg[0];
    int id = msg[1];
    msg = &msg[2];
    size -= 2;

    if (src == ID_INTERNAL) {  // requests queued for this thread: StcK, MIDI in, its tick
        if (id == IDI_SETTINGS_KEY && size >= 1) settings_key_press(msg[0]);
        if (id == IDI_MIDI_IN && size >= 3) midi_in_message(msg[0], msg[1], msg[2]);
        if (id == IDI_MIDI_IN_TICK) dis.synth_in.tick(dis);
        if (id == IDI_UMP_OUT_RETRY) (void)dis.ump_offs_flush();
        if (id == IDI_UMP_IN && size >= 4) {
            const auto word = [](int hi, int lo) { return (uint32_t)(uint16_t)hi << 16 | (uint16_t)lo; };
            midi_in_message2(word(msg[0], msg[1]), word(msg[2], msg[3]));
        }
        return 0;
    }

    if (src == ID_CONTROL) {
        if (config.key_swap) {
            if (id == IDC_ALT) {
                id = IDC_PORTAMENTO;
            }
            else if (id == IDC_PORTAMENTO) {
                id = IDC_ALT;
            }
        }
        if (config.altkey_pedal && id == IDC_ALT) {
            id = IDC_PEDAL_1;
        }
        if (id == IDC_ALT) {
            dis.set_altmode(msg[0]);
            update_leds();
        }
        else if (id == IDC_PORTAMENTO) {
            // Portamento button
            dis.set_portamento(msg[0]);
            update_leds();
        }
        else if (id == IDC_PEDAL_EXP) {
            // Pedal
            int2float(msg, fmsg, size);
            *dis.synth_pedal() = fmsg[0];
            pedal.send(msg[0]>>4, dis.control_channel());
        }
        else if (id == IDC_PEDAL_1) {
            // Switch pedal 1 (tip)
            int val = config.pedal_sw1_values[msg[0]>0];
            *dis.synth_pedal() = (float)val/127;
            pedal.send(val*4, dis.control_channel());
        }
        else if (id == IDC_PEDAL_2) {
            // Switch pedal 2 (ring)
            dis.set_portamento(msg[0]);
            update_leds();
        }
        else if (id == IDC_OCT_DOWN || id == IDC_OCT_UP) {
            if (msg[0]) {
                aux_button_map |= 1<<id;
                if (aux_button_map == ((1<<IDC_OCT_UP) | (1<<IDC_OCT_DOWN))) {
                    if (dis.altmode & 1) {
                        // transpose reset
                        dis.set_note_offset(62);
                    } else {
                        // cancel last transpose and enable free transpose
                        dis.set_note_offset(note_offset_old);
                        dis.set_free_transpose_mode(1);
                    }
                } else {
                    note_offset_old = dis.start_note_offset;
                    // transpose by current tuning octave interval
                    float dif = dis.flipdir * dis.notegen0;
                    if (dis.altmode & 1) {
                        // transpose by minor second interval (3 octaves up 5 fifths down)
                        dif = dis.flipdir * (dis.notegen0 * 3 + dis.notegen1 * -5);
                    }
                    if (id == IDC_OCT_UP) {
                        dis.change_note_offset(dif);
                    }
                    if (id == IDC_OCT_DOWN) {
                        dis.change_note_offset(-dif);
                    }
                }
            } else {
                aux_button_map &= ~(1<<id);
                if (aux_button_map != ((1<<IDC_OCT_UP) | (1<<IDC_OCT_DOWN))) {
                    dis.set_free_transpose_mode(0);
                }
                update_leds();
            }
        }
    }
    else if (src == ID_ACCEL && size == 7) {
        motion.message(msg);
    }
    else if ((src == ID_DIS || src == ID_BAS) && size == 4 && id < BUTTONCOUNT && id >= 0) {
        // 4x14bit value, dis or bas button, version >= 2.1

        int2float(msg, fmsg, size);

        if (src == ID_DIS) {
            dis.button_message(id, fmsg);
            dis.sync_key_states();
        }
    }
    else {
        // unknown message source
        return -1;
    }
    return 0;
}

// The instrument as PresetLoader sees it (preset_loader.hpp): the control logic's state and effects
struct InstrumentBoard {
    Instrument& in;

    void led(uint32_t rgb) { led_rgb(rgb); }
    void message_interval(int ms) { config.message_interval = ms; }
    void motion_interval(int i) { config.send_motion_interval = i; }
    midi_mode_t midi_mode() const { return config.midi_mode; }
    void set_midi_mode(midi_mode_t m) { ::set_midi_mode(m); }
    void set_midi2() { ::set_midi2(); }
    void midinote_mode(midinote_mode_t m) { config.midinote_mode = m; }
    void jack2(jack2_mode_t m) { aux_jack_switch_mode_setting(m); }
    int flipdir() const { return in.flipdir; }
    void flip() { in.flip(); }
    void load_tuning(int n) { in.load_tuning(n); }
    void preset_offset(float semitones) { in.note_offset = in.tuning_note_offset + semitones + in.midi_in.bend(); }
    void axis_cc(MidiAxis a, bool normal, int cc) {
        uint8_t& v = a == MidiAxis::pressure ? (normal ? config.midi_pres : config.mpe_pres)
                   : a == MidiAxis::x        ? (normal ? config.midi_x : config.mpe_x)
                                             : (normal ? config.midi_y : config.mpe_y);
        v = cc;
    }
    void contvelo(int cc) { config.mpe_contvelo = cc; }
    int channel_offset() const { return in.midi_channel_offset; }
    void channel_offset(int c) { in.midi_channel_offset = c; }
    void bend_range(int semitones) { in.midi_bend_range = semitones; }
    int voicecount() const { return in.voicecount; }
    void voicecount(int n) { in.voicecount = n; }
    void channels_changed() { in.channels_changed(); }
    void zero_offset(int v) { config.zero_offset = v; }
    void bend_sensitivity(float f) { in.bend_sensitivity = f; }
    void pres_sensitivity(float f) { in.pres_sensitivity = f; }
    void velo_sensitivity(float f) { in.velo_sensitivity = f; }
    void tilt_sensitivity(float f) { in.y_sensitivity = f; }
    void velo_offset(int v) { in.midi_velo_offset = v; }
    void volume(float v) { set_volume(v); }
    void decay(float v) {
        config.pedal_sw1_values[0] = v;
        *in.synth_pedal() = v * (1.0f/127.0f);
    }
    void pedal_decay(float v) { config.pedal_sw1_values[1] = v; }
    void midi_config() { ::midi_config(); }
    int voice_channel(int i) const { return in.voice_channel(i); }
    void program_change(int channel, int program) { MidiSend2(MIDI_PROGRAM_CHANGE | channel, program); }

    int tuning() const { return in.cur_tuning; }
    int tuning_color() const { return in.tuning_color; }
    void equal_temperament() {
        in.notegen0 = 12.0f;
        in.set_notegen1(7.0f);
        in.reset_note_offsets();
        in.tuning_note_offset = 0.0f;
        in.note_offset = in.tuning_note_offset + in.midi_in.bend();
    }
    void generators(float fifth, float octave) {
        in.notegen1 = fifth;
        in.notegen0 = octave;
    }
    void tuning_offset(float semitones) {
        in.tuning_note_offset = semitones;
        in.note_offset = in.tuning_note_offset + in.midi_in.bend();
    }
    // the key that plays note k: on a flipped layout, the mirrored one (SC-12)
    void key_offset(int k, float semitones) {
        in.buttons[button_number_map[in.flipdir == 1 ? k : 60 - k]].tuning_note_offset = semitones;
    }
    void set_tuning(int n, int color) {
        in.tuning_color = color;
        in.cur_tuning = n;
    }
};

void Instrument::load_tuning(int n) {
    InstrumentBoard b{*this};
    PresetLoader<InstrumentBoard>(settings::board(), b).load_tuning(n);
}

void load_preset(int n) {
    InstrumentBoard b{dis};
    PresetLoader<InstrumentBoard>(settings::board(), b).load_preset(n);
}


/*
 * The settings keys (settings_keys.hpp): one row per key that does something with Settings held
 * (ascending id), its layout entry and its action. The console prints the layout (StcX,
 * synth_settings_keys_print) so the PC shows the board's own instead of a copy; StcK presses the
 * rows flagged kConsole. A voice key is one row: its engine and the indicator colour config_voice
 * shows. A voice key whose engine isn't in this firmware is left out (StcX) and refused (StcK).
 */
using settings_keys::Action;
using settings_keys::kSwitch;
using settings_keys::kDial;
using settings_keys::kActionKind;
using settings_keys::kMidi;
using settings_keys::kLayout;
using settings_keys::kSens;
using settings_keys::kVoice;
using settings_keys::kTuning;
using settings_keys::kPreset;
using settings_keys::kMidi2;
using settings_keys::kSystem;
using settings_keys::kConsole;
using settings_keys::kDanger;

#define SK_VOICE_KNOB "step through its presets (the up/down leds show which)"
#define SK_TUNING_KNOB "fine tuning offset, 1/32 semitone per step"
static constexpr settings_keys::Key settings_key_table[] = {
    {0, kSwitch, kMidi, kConsole, -1, nullptr, 0, "MPE", "MPE MIDI mode", "number of MPE voices, 1 to 15", Action::mpe},
    {2, kSwitch, kMidi, kConsole, -1, nullptr, 0, "Poly", "Normal MIDI mode (all notes on one channel)", "MIDI channel, 1 to 16", Action::normal},
    {4, kSwitch, kMidi, kConsole, -1, nullptr, 0, "Mono", "Mono (glissando) MIDI mode", "MIDI channel, 1 to 16", Action::mono},
    {6, kDial, kSens, 0, -1, nullptr, 0, "Bend", "Pitch bend sensitivity (sideways tilt)", "0 to 4, steps of 1/4", Action::bend},
    {8, kDial, kSens, 0, -1, nullptr, 0, "Tilt", "Tilt (forward/back) sensitivity", "0 to 3, steps of 1/8", Action::tilt},
    {11, kActionKind, kLayout, 0, -1, nullptr, 0, "Flip", "Flip the layout 180 degrees (with the glissando button also held: debug mode on/off)", "", Action::flip},
#ifdef USE_INTERNAL_SYNTH
    {13, kSwitch, kVoice, kConsole, -1, "brass", 0x402800, "brass", "Voice: brass", SK_VOICE_KNOB, Action::voice},
    {15, kSwitch, kVoice, kConsole, -1, "trumpet", 0x003030, "tpt", "Voice: trumpet", SK_VOICE_KNOB, Action::voice},
#endif
    {17, kDial, kSens, 0, -1, nullptr, 0, "Thres", "Note-on threshold", "how light a touch starts a note (led dial 0 to 15)", Action::threshold},
#ifdef USE_INTERNAL_SYNTH
    {18, kSwitch, kVoice, kConsole, -1, "strisy", 0x282828, "strisy", "Voice: strisy", SK_VOICE_KNOB, Action::voice},
#endif
    {19, kDial, kSens, 0, -1, nullptr, 0, "Press", "Pressure sensitivity", "0 to 3, steps of 1/8", Action::pressure},
#ifdef USE_INTERNAL_SYNTH
    {20, kSwitch, kVoice, kConsole, -1, "va", 0x401400, "va", "Voice: va", SK_VOICE_KNOB, Action::voice},
#endif
    {21, kDial, kSens, 0, -1, nullptr, 0, "Velo", "Velocity sensitivity", "0 to 2, steps of 1/8", Action::velocity},
#ifdef USE_INTERNAL_SYNTH
    {22, kSwitch, kVoice, kConsole, -1, "modal", 0x001840, "modal", "Voice: modal", SK_VOICE_KNOB, Action::voice},
#endif
    {23, kSwitch, kTuning, kConsole, 3, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
#ifdef USE_INTERNAL_SYNTH
    {24, kSwitch, kVoice, kConsole, -1, "bowed", 0x400000, "bowed", "Voice: bowed", SK_VOICE_KNOB, Action::voice},
#endif
    {25, kSwitch, kTuning, kConsole, 4, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
#ifdef USE_INTERNAL_SYNTH
    {26, kSwitch, kVoice, kConsole, -1, "wind", 0x004000, "wind", "Voice: wind", SK_VOICE_KNOB, Action::voice},
#endif
    {27, kSwitch, kTuning, kConsole, 5, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
#ifdef USE_INTERNAL_SYNTH
    {28, kSwitch, kVoice, kConsole, -1, "ep", 0x280040, "ep", "Voice: ep", SK_VOICE_KNOB, Action::voice},
#endif
    {29, kSwitch, kTuning, kConsole, 6, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {30, kSwitch, kPreset, kConsole, 7, nullptr, 0, "P7", "Load settings preset 7", "", Action::preset},
    {31, kSwitch, kTuning, kConsole, 7, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {32, kSwitch, kPreset, kConsole, 8, nullptr, 0, "P8", "Load settings preset 8", "", Action::preset},
    {33, kSwitch, kTuning, kConsole, 8, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {34, kSwitch, kTuning, kConsole, 0, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {35, kSwitch, kPreset, kConsole, 1, nullptr, 0, "P1", "Load settings preset 1", "", Action::preset},
    {36, kSwitch, kTuning, kConsole, 1, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {37, kSwitch, kPreset, kConsole, 2, nullptr, 0, "P2", "Load settings preset 2", "", Action::preset},
    {38, kSwitch, kTuning, kConsole, 2, nullptr, 0, "", "", SK_TUNING_KNOB, Action::tuning},
    {39, kSwitch, kPreset, kConsole, 3, nullptr, 0, "P3", "Load settings preset 3", "", Action::preset},
    {41, kSwitch, kPreset, kConsole, 4, nullptr, 0, "P4", "Load settings preset 4", "", Action::preset},
    {43, kSwitch, kPreset, kConsole, 5, nullptr, 0, "P5", "Load settings preset 5", "", Action::preset},
    {45, kSwitch, kPreset, kConsole, 6, nullptr, 0, "P6", "Load settings preset 6", "", Action::preset},
    {51, kDial, kMidi2, 0, -1, nullptr, 0, "Vol", "Volume (MIDI CC7)", "volume, 0 to 127", Action::volume},
    {53, kDial, kMidi2, 0, -1, nullptr, 0, "Sust", "Sustain / decay (pedal)", "release length, 0 to 1", Action::sustain},
    {56, kDial, kMidi2, 0, -1, nullptr, 0, "MPEbd", "MPE pitch bend range", "12, 24, 48 or 96 semitones", Action::bend_range},
    {58, kDial, kMidi2, 0, -1, nullptr, 0, "Rate", "MIDI message interval", "1 to 15 ms", Action::rate},
    {63, kSwitch, kSystem, kConsole, -1, nullptr, 0, "Panic", "Panic: all MIDI notes off", "", Action::panic},
    {65, kActionKind, kSystem, 0, -1, nullptr, 0, "Cfg", "Send the MPE and pitch bend configuration over MIDI", "", Action::send_config},
    {67, kActionKind, kSystem, kDanger, -1, nullptr, 0, "Reset", "System reset: restarts the Striso", "", Action::reset},
};
static_assert(settings_keys::well_formed(settings_key_table));

// the tunings of the settings card (config.h fT<n>..., tuning 0 is built in): their names while the
// config has the factory values (sT<n>name, fT<n>fifth, fT<n>oct)
static const char* const tuning_names[9][2] = {
    {"12-TET", "12TET"}, {"31-TET", "31TET"}, {"19-TET", "19TET"}, {"Pythagorean", "Pyth"}, {"5-TET", "5TET"},
    {"7-TET", "7TET"}, {"Bohlen-Pierce 1", "BP1"}, {"Bohlen-Pierce 2", "BP2"}, {"Just 7-limit", "Just"}};

static const settings_keys::Key* settings_key_find(int id) { return settings_keys::find(settings_key_table, id); }

// in this firmware: every key but a voice key whose engine isn't built
static bool settings_key_present(const settings_keys::Key* k) {
#ifdef USE_INTERNAL_SYNTH
    if (!k->engine) return true;
    for (int n = 0; n < synth_num_voices(); n++)
        if (strcmp(synth_voice_name(n, NULL), k->engine) == 0) return true;
    return false;
#else
    return k->engine == NULL;
#endif
}

static bool settings_key_allowed(int id) {
    const settings_keys::Key* k = settings_key_find(id);
    return k && (k->flags & kConsole) && settings_key_present(k);
}

// a config value (8 characters) as text: trailing spaces off, '|' and control characters as '?';
// as a label (no_spaces) without spaces and at most 6 characters
static void setting_text(const char* v, char* out, bool no_spaces) {
    int n = 0, end = 0;
    for (int i = 0; i < 8 && (!no_spaces || n < 6); i++) {
        char c = v[i];
        if (c == ' ' && no_spaces) continue;
        if (c < 32 || c >= 127 || c == '|') c = '?';
        out[n++] = c;
        if (c != ' ') end = n;
    }
    out[end] = 0;
}

// the board's setting key as text (setting_text); out of line, as the next: StcX runs on main's
// 1 kB stack, and these keep the table and its values out of its frame
__attribute__((noinline)) static void config_setting_text(settings::KeyName key, char* out, bool no_spaces) {
    setting_text(settings::board().raw(key).value_or("        ").data(), out, no_spaces);
}

// key's value is its default
__attribute__((noinline)) static bool config_is_default(settings::KeyName key) {
    return settings::board().is_default(key);
}

// StcX: the layout, "X keys <n>", one line per key present (ascending id), "X end"; line format:
// X <id> <kind> <group> <engine|-> <led rrggbb|-> <flags|-> <label>|<title>|<knob>
void synth_settings_keys_print(void* chp) {
    BaseSequentialStream* out = (BaseSequentialStream*)chp;
    static const char* const kinds[] = {"switch", "dial", "action"};
    static const char* const groups[] = {"midi", "layout", "sens", "voice", "tuning", "preset", "midi2", "system"};
    int count = 0;
    for (const settings_keys::Key& k : settings_key_table) count += settings_key_present(&k);
    chprintf(out, "X keys %d\r\n", count);
    // static: only the console (the main thread) prints the layout, and main's stack is 1 KB;
    // each is written before it is read
    static char label[8], name[12], title[112];
    for (const settings_keys::Key& key : settings_key_table) {
        const settings_keys::Key* k = &key;
        if (!settings_key_present(k)) continue;
        char led[8] = "-", flags[4] = "-";
        if (k->engine) chsnprintf(led, sizeof led, "%06lx", (unsigned long)k->led);
        int f = 0;
        if (settings_key_allowed(k->id)) flags[f++] = 'k';
        if (k->flags & kDanger) flags[f++] = 'd';
        if (f) flags[f] = 0;
        const char* lab = k->label;
        const char* tit = k->title;
        using settings::KeyName;
        if (k->group == kTuning) {
            const int n = k->arg;
            bool factory = n == 0;  // tuning 0 is built in
            if (!factory) {
                factory = config_is_default(KeyName::tuning(n, "name")) &&
                          config_is_default(KeyName::tuning(n, "fifth")) && config_is_default(KeyName::tuning(n, "oct"));
            }
            if (factory) {
                lab = tuning_names[n][1];
                chsnprintf(title, sizeof title, "Tuning %d: %s", n, tuning_names[n][0]);
            } else {
                config_setting_text(KeyName::tuning(n, "name"), label, true);
                config_setting_text(KeyName::tuning(n, "name"), name, false);
                if (!label[0]) strcpy(label, "T?");
                lab = label;
                chsnprintf(title, sizeof title, "Tuning %d: %s (from the config)", n, name);
            }
            tit = title;
        } else if (k->group == kPreset) {
            // a preset named in the config (sP<n>name) other than its factory name
            if (!config_is_default(KeyName::preset(k->arg, "name"))) {
                config_setting_text(KeyName::preset(k->arg, "name"), name, false);
                chsnprintf(title, sizeof title, "%s: %s", k->title, name);
                tit = title;
            }
        }
        chprintf(out, "X %d %s %s %s %s %s %s|%s|%s\r\n", k->id, kinds[k->kind], groups[k->group],
                 k->engine ? k->engine : "-", led, flags, lab, tit, k->knob);
    }
    chprintf(out, "X end\r\n");
}

#ifdef USE_INTERNAL_SYNTH
/*
 * Synth voice selection with the settings row 4 ("future use" on the v2.2 settings card): each key
 * picks a voice engine, shown by the indicator led colour. On an engine with presets (the VA) the
 * key works like the other value keys: nudge it or circle it to step through the presets, the
 * up/down leds show which one. The last preset of each engine is remembered while powered.
 * Returns the unused part of adjust, like config_but.
 */
static float config_voice(const char* engine, int r, int g, int b, int type, float adjust) {
    static int8_t remembered[128];  // preset per engine, indexed by the engine's first entry (< 128)
    int first = -1, count = 0;
    for (int n = 0; n < synth_num_voices(); n++) {
        const char* preset;
        if (strcmp(synth_voice_name(n, &preset), engine) == 0) {
            if (first < 0) first = n;
            count++;
        }
    }
    if (count == 0 || first >= 128) {
        led_rgb(led_view::kNoEngine);
        return 0;
    }
    int cur = synth_current_voice();
    int idx = (cur >= first && cur < first + count) ? cur - first : remembered[first];
    if (idx >= count) idx = 0;
    int a = 0;
    if (type > 0) {
        a = (int)adjust;
        if (type == 2) a = a / 2;
        idx = clamp(idx + a, 0, count - 1);
        if (type == 2) a = a * 2;
    }
    if (type == 0 || a != 0) {
        synth_select_voice(first + idx);
        remembered[first] = idx;
    }
    led_rgb3(r, g, b);
    led_updown_dial(led_view::voice_dial(idx, count));
    return type > 0 ? adjust - a : 0;
}
#endif

/*
 * The settings keys' actions (settings_keys.hpp Action), with the Settings button held.
 *
 * type can be 0: initial press, 1: nudge up/down, 2: circular adjust
 *
 * returns part of adjust that's not used and hence needs to be saved for next call
 */
struct SettingsKeyActions {
    using Key = settings_keys::Key;

    // row 1:  0  2  4
    float mpe(const Key&, int type, float adjust) { // MPE MIDI mode, knob: number of MPE channels
        if (type == 0) {
            if (config.midi_mode != MIDI_MODE_MPE) set_midi_mode(MIDI_MODE_MPE);
            led_updown_dial(dis.voicecount);
            midi_config();
        } else {
            int a = (int)adjust;
            if (type == 2) a = a / 2;
            if (a != 0) {
                dis.voicecount = clamp(dis.voicecount + a, 1, 15);
                dis.channels_changed();
                led_updown_dial(dis.voicecount);
                midi_config();
            }
            if (type == 2) a = a * 2;
            return adjust - a;
        } return 0;
    }
    float normal(const Key&, int type, float adjust) { // Normal MIDI mode
        return channel_mode(MIDI_MODE_POLY, type, adjust);
    }
    float mono(const Key&, int type, float adjust) { // Mono (glissando) MIDI mode
        return channel_mode(MIDI_MODE_MONO, type, adjust);
    }
    // a one-channel mode, knob: the channel
    float channel_mode(midi_mode_t mode, int type, float adjust) {
        if (type == 0) {
            if (config.midi_mode != mode) set_midi_mode(mode);
            led_updown_dial(dis.midi_channel_offset + 1);
            midi_config();
        } else {
            int a = (int)adjust;
            if (type == 2) a = a / 2;
            if (a != 0) {
                dis.midi_channel_offset = clamp(dis.midi_channel_offset + a, 0, 15);
                dis.channels_changed();
                led_updown_dial(dis.midi_channel_offset + 1);
                midi_config();
            }
            if (type == 2) a = a * 2;
            return adjust - a;
        } return 0;
    }
    // row 2:  1  3  5  7  9 11
    float flip(const Key&, int type, float) { // debug setting for easy testing
        if (type == 0) {
            if (dis.portamento & 1) {
                if (config.debug) {
                    config.debug = 0;
                    // led_updown(0xf000);
                } else {
                    config.debug = 1;
                    // led_updown(0xffff);
                }
            } else {
                dis.flip();
            }
        } return 0;
    }
    // row 3: 17 19 21  6  8 10 12 14 16
    float threshold(const Key&, int type, float adjust) { // knob: key detection threshold
        int a = adjust * ((1 << 24) / 256);
        if (type > 0) {
            config.zero_offset = clamp(config.zero_offset + a, min(0, config.zero_offset),
                                       max(16 * (1 << 24) / 256 - 1, config.zero_offset));
        }
        const int dial = led_view::threshold_dial(config.zero_offset);
        led_updown_dial(dial);
        led_rgb3(led_view::threshold_red(dial), led_view::threshold_green(dial), 0);
        return 0; // TODO: sticky ends
    }
    float pressure(const Key&, int type, float adjust) { // knob: pres sensitivity
        float rem = 0;
        if (type > 0) {
            dis.pres_sensitivity = clamp_knob(dis.pres_sensitivity, dis.pres_sensitivity + adjust * (1.0f/8.0f), 0, 3.0f, &rem);
        }
        led_updown_dial(led_view::pressure_dial(dis.pres_sensitivity));
        led_rgb3(led_view::pressure_level(dis.pres_sensitivity), led_view::pressure_level(dis.pres_sensitivity), 0);
        return rem * 8;
    }
    float velocity(const Key&, int type, float adjust) { // knob: velo sensitivity
        float rem = 0;
        if (type > 0) {
            dis.velo_sensitivity = clamp_knob(dis.velo_sensitivity, dis.velo_sensitivity + adjust * (1.0f/8.0f), 0, 2.0f, &rem);
        }
        led_updown_dial(led_view::velocity_dial(dis.velo_sensitivity));
        led_rgb3(led_view::velocity_level(dis.velo_sensitivity), 0, led_view::velocity_level(dis.velo_sensitivity));
        return rem * 8;
    }
    float bend(const Key&, int type, float adjust) { // knob: pitchbend range
        float rem = 0;
        if (type > 0) {
            dis.bend_sensitivity = clamp_knob(dis.bend_sensitivity, dis.bend_sensitivity + adjust * 0.25f, 0, 4.0f, &rem);
        }
        led_updown_dial(led_view::bend_dial(dis.bend_sensitivity));
        led_rgb3(led_view::bend_level(dis.bend_sensitivity), 0, 0);
        return rem * 4;
    }
    float tilt(const Key&, int type, float adjust) { // knob: y sensitivity (TODO: flip?)
        float rem = 0;
        if (type > 0) {
            dis.y_sensitivity = clamp_knob(dis.y_sensitivity, dis.y_sensitivity + adjust * (1.0f/8.0f), 0, 3.0f, &rem);
        }
        led_updown_dial(led_view::tilt_dial(dis.y_sensitivity));
        led_rgb3(led_view::tilt_red(dis.y_sensitivity), 0, led_view::tilt_blue(dis.y_sensitivity));
        return rem * 8;
    }
    // row 4: 18 20 22 24 26 28 13 15 ("future use" on the v2.2 card): the row's synth voice
    // engine and indicator colour (strisy grey, va orange, modal blue, bowed red, wind green,
    // ep purple, brass amber, trumpet cyan)
    float voice(const Key& k, int type, float adjust) {
#ifdef USE_INTERNAL_SYNTH
        return config_voice(k.engine, k.led >> 16, (k.led >> 8) & 0xff, k.led & 0xff, type, adjust);
#else
        (void)k;
        (void)type;
        (void)adjust;
        return 0;
#endif
    }
    // row 5: 34 36 38 23 25 27 29 31 33
    float tuning(const Key& k, int type, float adjust) { // load tuning k.arg, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(k.arg);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(led_view::tuning_dial(dis.note_offset));
        return 0;
    }
    // row 6: 35 37 39 41 43 45 30 32
    float preset(const Key& k, int type, float) { // load settings preset k.arg
        if (type == 0) {
            load_preset(k.arg);
        } return 0;
    }
    // row 7: 51 53 55 40 42 44 46 48 50
    float volume(const Key&, int type, float adjust) { // knob: Volume (CC7)
        float rem = 0;
        if (type > 0) {
            float vol = clamp_knob(volume_linear, volume_linear + adjust * 8.0f, 0, 127.0f, &rem);
            if ((int)(volume_linear + 0.5f) != (int)(vol + 0.5f)) {
                MidiSend3(MIDI_CONTROL_CHANGE | dis.control_channel(), MIDI_C_MAIN_VOLUME, vol + 0.5f);
            }
            set_volume(vol);
        }
        led_rgb3(led_view::volume_level(volume_linear), led_view::volume_level(volume_linear), led_view::volume_level(volume_linear));
        led_updown_dial(led_view::volume_dial(volume_linear));
        return rem * (1.0f/8.0f);
    }
    float sustain(const Key&, int type, float adjust) { // knob: sustain/decay rate
        float rem = 0;
        if (type > 0) {
            *dis.synth_pedal() = clamp_knob(*dis.synth_pedal(), *dis.synth_pedal() + adjust * (1.0f/16.0f), 0, 1.0f, &rem);
            pedal.send(*dis.synth_pedal() * 128 * 4, dis.control_channel());
        }
        led_updown_dial(led_view::sustain_dial(*dis.synth_pedal()));
        led_rgb3(led_view::sustain_level(*dis.synth_pedal()), 0, led_view::sustain_level(*dis.synth_pedal()));
        return rem * 16;
    }
    // row 8:       56 58 60 62 47 49
    float bend_range(const Key&, int type, float adjust) { // knob: MPE pitchbend range
        int a = adjust;
        // the ranges 12, 24, 48, 96; shown: the largest not above the range (12 below 12). The
        // range can be any 1..127 (a preset, MIDI in): log2i(range / 12) was log2i(0) below 12 (UB)
        // and the first step from 36 went to 24 or 48 the same way (SC-20)
        float r = dis.midi_bend_range;
        int cur = r >= 96 ? 3 : r >= 48 ? 2 : r >= 24 ? 1 : 0;
        if (type > 0) {
            if (type == 2) a = a / 4;
            if (a != 0) {
                int n = cur + a;
                if (a > 0 && r < (12 << cur)) n--;  // below 12: the first step up is to 12
                if (a < 0 && r > (12 << cur)) n++;  // between two: the first step down is to the lower
                n = clamp(n, 0, 3);
                if ((a > 0 && (12 << n) > r) || (a < 0 && (12 << n) < r)) {
                    cur = n;
                    dis.midi_bend_range = 12 << cur;
                    midi_config();
                }
            }
            if (type == 2) a = a * 4;
        }
        led_rgb3(led_view::bend_range_red(cur), 0, 0);
        led_updown_dial(led_view::bend_range_dial(cur));
        return adjust - a;
    }
    float rate(const Key&, int type, float adjust) { // knob: MIDI message interval
        int a = (int)adjust;
        if (type > 0) {
            config.message_interval = clamp(config.message_interval - a, 1, 15);
        }
        led_rgb3(led_view::rate_level(config.message_interval), led_view::rate_level(config.message_interval), 0);
        led_updown_dial(led_view::rate_dial(config.message_interval));
        return adjust - a;
    }
    // row 9:                   63 65 67
    float panic(const Key&, int type, float) { // panic/all notes off
        if (type == 0) {
            dis.panic();
            led_rgb(led_view::kPanic);
        } return 0;
    }
    float send_config(const Key&, int type, float) { // send MPE/pitch bend range config
        if (type == 0) {
            midi_config();
            led_rgb(led_view::kSendConfig);
        } return 0;
    }
    float reset(const Key&, int type, float) { // system reset
        if (type == 0) {
            led_rgb(led_view::kReset);
            reset_on_release = true;
        } return 0;
    }
};

/*
 * button functions in alt mode - with settings button pressed: the key's row in settings_key_table
 * and its action (a key without a row does nothing)
 */
float config_but(int but, int type, float adjust) {
    const settings_keys::Key* k = settings_key_find(but);
    if (!k) return 0;
    SettingsKeyActions a;
    return settings_keys::press(*k, a, type, adjust);
}

/*
 * MIDI in's configuration CCs overlap what the Striso sends, so a DAW's MIDI thru or a loop through
 * an interface that echoes the Striso's own output back reconfigured it (the tilt value became
 * the Y axis CC number, the motion CCs the message interval and the transposition, SC-16). An
 * echo is recognised by the pattern of that output, and ignored:
 * - on the channels of the notes: their pitch bend and their pressure, x, y and continuous
 *   velocity CCs;
 * - on channel 1: a motion CC (16, 17, 18) with a value sent in the last 0.5 s.
 */
static bool note_channel(int channel) {
    if (config.midi_mode == MIDI_MODE_MPE) {
        return channel >= dis.midi_channel_offset && channel < dis.midi_channel_offset + dis.voicecount;
    }
    return channel == dis.midi_channel_offset;
}

static bool note_cc(int cc) {
    if (config.midi_mode == MIDI_MODE_POLY) {
        return cc == config.midi_pres || cc == config.midi_x || cc == config.midi_y;
    }
    return cc == config.mpe_pres || cc == config.mpe_x || cc == config.mpe_y
           || (config.mpe_contvelo < 120 && (cc == 72 || cc == 73));
}

/*
 * MIDI in (console.cpp's USB MIDI, on the main thread) goes to the thread that handles the keys
 * (synth_message, ID_INTERNAL), which owns the voices, the channels and the configuration: MIDI
 * in no longer changes them halfway through a key's message (SC-33). A full queue is waited for.
 */
void MidiInMsgHandler(midi_device_t dev, uint8_t port, uint8_t status,
                      uint8_t data1, uint8_t data2) {
    (void) dev;
    (void) port;
    for (int n = 0; !messaging::send(Source::internal, IDI_MIDI_IN, status, data1, data2) && n < 50; n++) {
        chThdSleepMilliseconds(1);
    }
}

/*
 * MIDI 2.0 in (sv_ump_write; later USB MIDI 2.0), whole packets: a MIDI 1.0 message (types 1, 2)
 * is MidiInMsgHandler's, a MIDI 2.0 channel voice message (type 4) goes to the same thread as
 * MIDI in (IDI_UMP_IN: its words as four 16-bit halves, the queue's values), the rest is skipped.
 */
int UmpInMsgHandler(midi_device_t dev, const uint32_t* words, int n) {
    int i = 0;
    while (i < n && i + striso::ump::words(words[i]) <= n) {
        const uint32_t w0 = words[i];
        const uint8_t type = (uint8_t)(w0 >> 28), port = (uint8_t)(((w0 >> 24) & 0x0f) + 1);
        if (type == 1 || type == 2) {
            MidiInMsgHandler(dev, port, (uint8_t)(w0 >> 16), (uint8_t)(w0 >> 8) & 0x7f, (uint8_t)w0 & 0x7f);
        } else if (type == 4 || type == 0xf) {  // (a Stream request: its first two words)
            const uint32_t w1 = words[i + 1];
            const auto half = [](uint32_t w) { return (int)(int16_t)(uint16_t)w; };
            for (int k = 0; !messaging::send(Source::internal, IDI_UMP_IN,
                     std::array<int, 4>{half(w0 >> 16), half(w0), half(w1 >> 16), half(w1)})
                 && k < 50; k++) {
                chThdSleepMilliseconds(1);
            }
        }
        i += striso::ump::words(w0);
    }
    return i;
}

/*
 * The host of MIDI in's instrument messages (striso::midi::InstrumentInput: the RPN select, the
 * bend range, the MCM, the instrument's bend, the pedal): what follows from them on the board.
 * (The notes' messages: dis is NoteInput's host.)
 */
struct MidiInInstrument {
    Instrument& d;
    // NoteInput's host: the Instrument's
    void ump(const striso::ump::Packet& p) { d.ump(p); }
    bool echoed(int channel) const { return d.echoed(channel); }
    float bend_range() const { return d.bend_range(); }
    // the MPE master channel's own range (2) is not the notes'
    bool zone_master(int channel) const {
        return config.midi_mode == MIDI_MODE_MPE && d.midi_channel_offset == 1 && channel == 0;
    }
    void set_bend_range(float semitones) { d.midi_bend_range = semitones; }
    // an MCM: the MIDI out goes MPE with that many channels. 0 (zone off) is ignored rather than
    // leaving no channel; the same configuration again (an echo of midi_config's) changes nothing.
    void zone_configured(int channel, int members) {
        if (channel == 0 && members >= 1 && members <= MAX_VOICECOUNT
            && !(config.midi_mode == MIDI_MODE_MPE && d.midi_channel_offset == 1 && d.voicecount == members)) {
            set_midi_mode(MIDI_MODE_MPE);
            d.midi_channel_offset = 1;
            d.voicecount = members;
            d.channels_changed();
        }
    }
    void set_pedal(float p) { *d.synth_pedal() = p; }
    // the channels the Striso sends its notes' bends on: their bend is an echo (SC-16)
    bool bends_out_on(int channel) const { return note_channel(channel); }
    // the instrument's bend is on top of the tuning's offset, the preset's and the fine tune (it
    // replaced them all, SC-15)
    void bend_moved(float delta) { d.note_offset += delta; }
};

// the bend range, the MCM, the instrument's bend, the pedal (and the RPN select per channel)
static striso::midi::InstrumentInput instrument_in;

static void board_cc(uint8_t channel, uint8_t data1, uint8_t data2);

// MIDI in's echo of the Striso's own MIDI out: its note data, its motion CCs
static bool echoed_cc(uint8_t channel, uint8_t cc, uint8_t value) {
    return (cc < 120 && note_channel(channel) && note_cc(cc))
           || (channel == 0 && cc >= 16 && cc <= 18 && motion.sent_recently(cc - 16, value));
}

static void midi_in_message(uint8_t status, uint8_t data1, uint8_t data2) {
    uint8_t channel = status & 0x0f;
    status &= 0xf0;

    // blink for channel messages; not for system messages (a DAW's clock, 24 per quarter note,
    // and active sensing kept the led pink, hiding the settings feedback, SC-37)
    if (status != 0xf0) {
        led_rgb3_blink(255, 0, 127, TIME_MS2I(100));
    }

    if (config.send_midi_monitor) {
        // Monitor MIDI input
        (void)messaging::send(Source::midi, channel, status >> 4, data1, data2);
    }

    if (status != 0xf0 && dis.midi_in.message(dis, channel, status, data1, data2)) {
        dis.synth_in.flush(dis);
        return;
    }
    if (status == MIDI_CONTROL_CHANGE && echoed_cc(channel, data1, data2)) {
        return;
    }

    MidiInInstrument host{dis};
    if (status != 0xf0 && instrument_in.message(host, dis.midi_in, channel, status, data1, data2)) {
        dis.synth_in.flush(dis);  // (the instrument's bend moved the MIDI notes)
        return;
    }

    if (status == MIDI_CONTROL_CHANGE) {
        board_cc(channel, data1, data2);
    }
}

/*
 * A MIDI 2.0 channel voice message (UMP type 4, any group), on the thread that handles the keys:
 * the notes and the instrument as MIDI 1.0's in their resolution (midi_in.h, message2), the
 * board's own CCs and the echo filter with the value's top 7 bits (MIDI 2.0's down-scaling).
 */
// UMP Stream (ump_stream.hpp): the endpoint as a MIDI 2.0 host discovers it, on USB MIDI 2.0
static ump_stream::Identity ump_identity(void) {
    static char serial[25];  // the board's serial, as StcV and the USB serial number give it
#ifdef UID_BASE
    if (serial[0] == '\0') {
        const uint32_t* uid = (const uint32_t*)UID_BASE;
        const uint32_t v[3] = {uid[2], uid[1], uid[0]};
        for (int i = 0; i < 24; i++) serial[i] = "0123456789ABCDEF"[(v[i / 8] >> (28 - 4 * (i % 8))) & 0xf];
    }
#endif
    return {"Striso", serial, {0x7d, 0x00, 0x00}, 0, 0, {0, 0, 0, 0},
            config.midi2 ? ump_stream::PROTOCOL_MIDI2 : ump_stream::PROTOCOL_MIDI1};
}

struct UmpStreamSink {
    void ump4(const uint32_t* w) { (void)midi_usb_UmpSend(w, 4); }
};

// a protocol change (the MIDI mode) goes to a MIDI 2.0 host unasked
static void ump_stream_changed(void) {
    static uint8_t was = ump_stream::PROTOCOL_MIDI1;  // the protocol in effect (told or not)
    const ump_stream::Identity id = ump_identity();
    const bool changed = id.protocol != was;
    was = id.protocol;
    if (!changed || !midi_usb_ump()) return;
    UmpStreamSink sink;
    ump_stream::configuration(sink, id);
}

static void midi_in_message2(uint32_t w0, uint32_t w1) {
    if ((w0 >> 28) == 0xf) {  // a Stream message: discovery, the stream's configuration
        const uint32_t w[4] = {w0, w1, 0, 0};
        UmpStreamSink sink;
        (void)ump_stream::reply(w, ump_identity(), sink);
        return;
    }
    const uint8_t channel = (uint8_t)((w0 >> 16) & 0x0f), status = (uint8_t)((w0 >> 16) & 0xf0);
    const uint8_t index = (uint8_t)((w0 >> 8) & 0x7f), value = (uint8_t)(w1 >> 25);

    led_rgb3_blink(255, 0, 127, TIME_MS2I(100));
    if (config.send_midi_monitor) {
        // the monitor's MIDI 1.0 view: velocities and values to 7 bits, the bend to 14
        const bool bend = status == MIDI_PITCH_BEND, chan_pres = status == MIDI_CHANNEL_PRESSURE;
        const int d1 = bend ? (int)((w1 >> 18) & 0x7f) : chan_pres ? value : index;
        const int d2 = chan_pres ? 0 : value;
        (void)messaging::send(Source::midi, channel, status >> 4, d1, d2);
    }

    if (dis.midi_in.message2(dis, w0, w1)) {
        dis.synth_in.flush(dis);
        return;
    }
    if (status == MIDI_CONTROL_CHANGE && echoed_cc(channel, index, value)) {
        return;
    }

    MidiInInstrument host{dis};
    if (instrument_in.message2(host, dis.midi_in, w0, w1)) {
        dis.synth_in.flush(dis);
        return;
    }

    if (status == MIDI_CONTROL_CHANGE) {
        board_cc(channel, index, value);
    }
}

// the board's own CCs (MIDI 1.0's and 2.0's, 7-bit)
static void board_cc(uint8_t channel, uint8_t data1, uint8_t data2) {
    switch (data1) {
        case  7: set_volume(data2); break;
        case 16: config.send_motion_interval = data2; break;
        // case 16|MIDI_C_LSB: config.send_motion_14bit = data2; break;
        case 17: config.message_interval = data2 >= 1 ? data2 : 1; break;
        // case 17|MIDI_C_LSB: config.send_button_14bit = data2; break;
        case 18: { // Note offset
            dis.set_note_offset(data2 - 2);
            update_leds();
        } break;
        case 65: { // portamento (glissando): on at >= 64, as MIDI's switches (SC-29)
            dis.set_portamento(data2 >= 64);
            update_leds();
        } break;
        case 70: { // pressure
            if (data2 == 0) data2 = CFG_DISABLE;
            if (config.midi_mode == MIDI_MODE_MPE) {
                config.mpe_pres = data2;
            } else {
                config.midi_pres = data2;
            }
        } break;
        case 71: { // x
            if (data2 == 0) data2 = CFG_DISABLE;
            if (config.midi_mode == MIDI_MODE_MPE) {
                config.mpe_x = data2;
            } else {
                config.midi_x = data2;
            }
        } break;
        case 72: { // cont release velo
        } break;
        case 73: { // cont velo
        } break;
        case 75: { // pitch bend sensitivity, TODO: different CC?
            dis.bend_sensitivity = (float)data2 * 0.05;
        } break;
        case MIDI_C_MONO: { // MPE/multitimbral/mono mode.
            // Set base channel based on channel the message is received
            if (data2 == 1) { // monophonic on one channel
                set_midi_mode(MIDI_MODE_MONO);
                dis.midi_channel_offset = channel;
            } else { // MPE like without master channel
                set_midi_mode(MIDI_MODE_MPE);
                dis.midi_channel_offset = channel;
                if (data2 > 0) {
                    dis.voicecount = data2;
                }
            }
            dis.channels_changed();
        } break;
        case MIDI_C_POLY: { // Polyphonic mode on the channel the message is received
            set_midi_mode(MIDI_MODE_POLY);
            dis.midi_channel_offset = channel;
            dis.channels_changed();
        } break;
        default: break;
    }
}

static bool setting_midi2 = false;

// MIDI 2.0 out over USB (console Stcq): UMPs the full queue dropped, held back behind a waiting
// Note Off, Note Offs that had to wait, and those waiting now (none is ever dropped)
void synth_ump_out_stats(uint32_t* drops, uint32_t* held_back, uint32_t* offs_retried, int* offs_pending) {
    *drops = midi_usb_ump_drops();
    *held_back = dis.ump_held_back;
    *offs_retried = dis.ump_offs_retried;
    *offs_pending = dis.ump_offs_pending();
}

// MIDI mode midi2: MPE on MIDI 1.0 (DIN, USB's MIDI 1.0 alt setting), MIDI 2.0 on USB's MIDI 2.0
// alt setting
void set_midi2(void) {
    setting_midi2 = true;  // (one notification: MIDI 2.0's, not MPE's first)
    set_midi_mode(MIDI_MODE_MPE);
    setting_midi2 = false;
    config.midi2 = true;
    ump_stream_changed();
}

void set_midi_mode(midi_mode_t mode) {
    config.midi2 = false;  // (set_midi2 sets it again)
    // the notes of the old mode end in channels_changed (below)
    switch (mode) {
        case MIDI_MODE_MPE: {
            config.midi_mode = mode;
            dis.midi_channel_offset = 1;
            dis.voicecount = VOICECOUNT;
            dis.portamento = 0;
            ws2812_write_led(0, 0, 16, 0);
        } break;
        case MIDI_MODE_POLY: {
            config.midi_mode = mode;
            dis.midi_channel_offset = 0;
            dis.portamento = 0;
            ws2812_write_led(0, 8, 1, 0);
        } break;
        case MIDI_MODE_MONO: {
            config.midi_mode = mode;
            dis.midi_channel_offset = 1;
            dis.voicecount = 1;
            dis.portamento = 2; // 2 keeps led from brightening
            ws2812_write_led(0, 0, 1, 18);
        } break;
    }
    dis.channels_changed();
    if (!setting_midi2) ump_stream_changed();
}

void midi_config(void) {
#ifdef USE_MIDI_OUT
    if (config.midi_mode == MIDI_MODE_MPE) {
        if (dis.midi_channel_offset == 1) {
            // valid MPE should be on channel 1 (or 16 which is not implemented)
            MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_RPN_MSB, 0);
            MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_RPN_LSB, 6); // mpe set voicecount
            MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_DATA_ENTRY, dis.voicecount);
            MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_RPN_MSB, 0x7f); // reset RPN
            MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_RPN_LSB, 0x7f);
        }
        for (int n = 0; n < dis.voicecount; n++) {
            // send bend range on all channels to be more compatible
            MidiSend3(MIDI_CONTROL_CHANGE | dis.voice_channel(n), MIDI_C_RPN_MSB, 0);
            MidiSend3(MIDI_CONTROL_CHANGE | dis.voice_channel(n), MIDI_C_RPN_LSB, 0); // bend range
            MidiSend3(MIDI_CONTROL_CHANGE | dis.voice_channel(n), MIDI_C_DATA_ENTRY, dis.midi_bend_range);
            MidiSend3(MIDI_CONTROL_CHANGE | dis.voice_channel(n), MIDI_C_RPN_MSB, 0x7f); // reset RPN
            MidiSend3(MIDI_CONTROL_CHANGE | dis.voice_channel(n), MIDI_C_RPN_LSB, 0x7f);
        }
    } else if (config.midi_mode == MIDI_MODE_MONO) {
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_MSB, 0);
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_LSB, 0); // bend range
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_DATA_ENTRY, dis.midi_bend_range);
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_MSB, 0x7f); // reset RPN
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_LSB, 0x7f);
    } else if (config.midi_mode == MIDI_MODE_POLY) {
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_MSB, 0);
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_LSB, 0); // bend range
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_DATA_ENTRY, 2); // in poly mode pitch bend range is 2 semitones
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_MSB, 0x7f); // reset RPN
        MidiSend3(MIDI_CONTROL_CHANGE | dis.midi_channel_offset, MIDI_C_RPN_LSB, 0x7f);
    }
#endif
}

// Live global touch parameters (Striso Live, parameter target 2). Like the settings keys, changes
// apply immediately and aren't saved on the board; the PC tools save them as preset 1 values
// (fP1presS, fP1veloS, fP1tiltS, fP1bendS, fP1thres) with iGpreset = 1.
static const char* const touch_names[5] = {"pressure", "velocity", "tilt", "bend", "threshold"};
static const float touch_min[5] = {0.1f, 0.1f, -4.0f, 0.0f, 0.0f};
static const float touch_max[5] = {4.0f, 4.0f, 4.0f, 8.0f, 0.5f};

int synth_touch_count(void) {
    return 5;
}

const char* synth_touch_info(int i, float* lo, float* hi) {
    if (i < 0 || i >= 5) return 0;
    *lo = touch_min[i];
    *hi = touch_max[i];
    return touch_names[i];
}

float synth_touch_get(int i) {
    switch (i) {
    case 0: return dis.pres_sensitivity;
    case 1: return dis.velo_sensitivity;
    case 2: return dis.y_sensitivity;
    case 3: return dis.bend_sensitivity * 0.5f;   // as in the config (bendS is stored halved)
    case 4: return (float)config.zero_offset * (1.0f / (1 << 24));
    }
    return 0.0f;
}

int synth_touch_set(int i, float v) {
    if (i < 0 || i >= 5 || !(v == v)) return 0;
    v = clamp(v, touch_min[i], touch_max[i]);
    switch (i) {
    case 0: dis.pres_sensitivity = v; break;
    case 1: dis.velo_sensitivity = v; break;
    case 2: dis.y_sensitivity = v; break;
    case 3: dis.bend_sensitivity = v * 2.0f; break;
    case 4: config.zero_offset = (int32_t)(v * (1 << 24)); break;
    }
    return 1;
}

// StcN: the active tuning and every key's pitch, for tuning-aware note labels (Striso Live).
// Read only (a tuning change in the middle of the print can mix old and new lines; print again).
// Line format: src/synth/tuning.h.
void synth_tuning_print(void* chp) {
    BaseSequentialStream* out = (BaseSequentialStream*)chp;
    const auto bits = [](float f) { return (unsigned long)std::bit_cast<uint32_t>(f); };
    striso::tuning::Tuning t;
    t.octave = dis.notegen0;
    t.fifth = dis.notegen1;
    t.offset = dis.note_offset;
    t.base = dis.start_note_offset;
    chprintf(out, "tuning %d %08lx %08lx %08lx %06x %08lx %d\r\n", dis.cur_tuning, bits(t.fifth),
             bits(t.octave), bits(t.offset), dis.tuning_color & 0xffffff, bits(t.base),
             striso::tuning::KEY_COUNT);
    for (int n = 0; n < striso::tuning::KEY_COUNT; n++) {
        char name[4];
        put_button_name(n, name);
        name[3] = 0;
        const Button& b = dis.buttons[button_number_map[n]];
        const float pitch = striso::tuning::key_pitch(t, b.tuning_note_offset, b.coord0, b.coord1);
        chprintf(out, "%d %s %08lx %08lx\r\n", n, name, bits(pitch), bits(b.tuning_note_offset));
    }
}

// --- StcY / Stcy: the board's state and its change counter --------------------------------------

// everything the console shows (StcY and the lists), as a snapshot to compare: no padding (all
// 32-bit words), so memcmp compares exactly the values
typedef struct {
    uint32_t params;  // synth_params_signature: the voice and every live parameter of targets 0, 1
    int32_t voice, midi_mode, channel_offset, voicecount, midinote_mode;
    float bend_range;
    int32_t tuning, tuning_color;
    float notegen0, notegen1, note_offset, tuning_note_offset, start_note_offset;
    uint32_t key_offsets;  // a hash of the keys' tuning offsets (N)
    uint32_t key_factors;  // a hash of the keys' sensitivity factors (k)
    float pres, velo, tilt, bend;
    int32_t zero_offset;
    float volume, pedal;
    int32_t message_interval, alt, portamento, flipdir, debug;
    int32_t led[3];
    uint32_t updown;
} observed_state_t;

static uint32_t hash_floats(uint32_t h, float f) {
    const uint32_t w = std::bit_cast<uint32_t>(f);
    for (int b = 0; b < 4; b++) h = (h ^ ((w >> (8 * b)) & 0xff)) * 16777619u;
    return h;
}

static void observe(observed_state_t* o) {
    memset(o, 0, sizeof *o);
#ifdef USE_INTERNAL_SYNTH
    o->params = synth_params_signature();
#endif
    o->voice = synth_current_voice();
    o->midi_mode = config.midi_mode;
    o->channel_offset = dis.midi_channel_offset;
    o->voicecount = dis.voicecount;
    o->midinote_mode = config.midinote_mode;
    o->bend_range = dis.midi_bend_range;
    o->tuning = dis.cur_tuning;
    o->tuning_color = dis.tuning_color;
    o->notegen0 = dis.notegen0;
    o->notegen1 = dis.notegen1;
    o->note_offset = dis.note_offset;
    o->tuning_note_offset = dis.tuning_note_offset;
    o->start_note_offset = dis.start_note_offset;
    uint32_t h = 2166136261u;
    for (int n = 0; n < BUTTONCOUNT; n++) h = hash_floats(h, dis.buttons[n].tuning_note_offset);
    o->key_offsets = h;
    h = 2166136261u;
    for (int n = 0; n < buttonKeyCount(); n++) h = hash_floats(h, buttonGetKeyFactor(n));
    o->key_factors = h;
    o->pres = dis.pres_sensitivity;
    o->velo = dis.velo_sensitivity;
    o->tilt = dis.y_sensitivity;
    o->bend = dis.bend_sensitivity;
    o->zero_offset = config.zero_offset;
    o->volume = volume_linear;
    o->pedal = dis.synth_interface && dis.synth_interface->pedal ? *(dis.synth_interface->pedal) : 0.0f;
    o->message_interval = config.message_interval;
    o->alt = dis.altmode & 1;
    o->portamento = dis.portamento;
    o->flipdir = dis.flipdir;
    o->debug = config.debug;
    for (int i = 0; i < 3; i++) o->led[i] = last_color[i];
    o->updown = led_updown_get();
}

static uint32_t state_counter = 0;
static observed_state_t state_last;

// Stcy: compares the state with the last look's; called from the console only (one thread)
uint32_t synth_state_counter(void) {
    observed_state_t o;
    observe(&o);
    if (memcmp(&o, &state_last, sizeof o) != 0) {
        state_last = o;
        state_counter++;
    }
    return state_counter;
}

// a float as text with 2 or 3 decimals, rounded (chprintf has no floats here)
static void put_fixed(char* buf, size_t size, float v, int decimals) {
    const int32_t scale = decimals == 2 ? 100 : 1000;
    if (!(v == v)) {
        chsnprintf(buf, size, "nan");
        return;
    }
    const bool neg = v < 0.0f;
    const float a = neg ? -v : v;
    const uint32_t q = a < 2.0e6f ? (uint32_t)(a * scale + 0.5f) : 2000000u * scale;
    const uint32_t ip = q / scale, fp = q % scale;
    if (q == 0) chsnprintf(buf, size, decimals == 2 ? "0.00" : "0.000");
    else if (decimals == 2) chsnprintf(buf, size, "%s%lu.%02lu", neg ? "-" : "", (unsigned long)ip, (unsigned long)fp);
    else chsnprintf(buf, size, "%s%lu.%03lu", neg ? "-" : "", (unsigned long)ip, (unsigned long)fp);
}

/*
 * StcY: the board's state in one line, the counter first (what Stcy says now):
 *   Z <counter> voice=<n> midi=<mpe|poly|mono> ch=<n> voices=<n> bend=<semitones> tuning=<index>
 *     fine=<cents> transpose=<semitones> pres=<f> velo=<f> tilt=<f> bendsens=<f> thres=<f>
 *     vol=<0..127> sustain=<f> rate=<ms> alt=<0|1> porta=<0|1> flip=<0|1> led=<rrggbb>
 *     updown=<up2 up down down2: 0 off, 1 dim, 2 on>
 * ch: the first channel the notes go to (1..16; MPE: the first member channel); voices: the MPE
 * voices; bend: the MPE pitch bend range; tuning: the loaded tuning (-1: a fifth set by MIDI CC1);
 * fine: the fine tuning (the knob, MIDI pitch bend) in cents above the tuning's own offset;
 * transpose: the transposition in semitones (octave buttons, free transpose; not a whole number in
 * other tunings); pres velo tilt bendsens thres: the touch parameters (target 2, as Stcg lists them);
 * vol: the volume (CC7); sustain: the synth's pedal value; rate: the MIDI message interval; alt: the
 * Settings button is held; porta: glissando on (mono mode keeps it on); led: the indicator colour as
 * last set (r g b, a blink not shown). Floats with 3 decimals, fine with 2.
 */
void synth_state_print(void* chp) {
    BaseSequentialStream* out = (BaseSequentialStream*)chp;
    const uint32_t counter = synth_state_counter();
    const observed_state_t& o = state_last;
    static const char* const modes[] = {"mpe", "poly", "mono"};
    // static: only the console (the main thread) prints the state, and main's stack is 1 KB
    static char fine[16], transpose[16], pres[16], velo[16], tilt[16], bend[16], thres[16], sustain[16];
    put_fixed(fine, sizeof fine, (o.note_offset - o.tuning_note_offset) * 100.0f, 2);
    put_fixed(transpose, sizeof transpose, o.start_note_offset - 62.0f, 3);
    put_fixed(pres, sizeof pres, synth_touch_get(0), 3);
    put_fixed(velo, sizeof velo, synth_touch_get(1), 3);
    put_fixed(tilt, sizeof tilt, synth_touch_get(2), 3);
    put_fixed(bend, sizeof bend, synth_touch_get(3), 3);
    put_fixed(thres, sizeof thres, synth_touch_get(4), 3);
    put_fixed(sustain, sizeof sustain, o.pedal, 3);
    int rgb[3];
    for (int i = 0; i < 3; i++) rgb[i] = clamp(o.led[i], 0, 255);
    char updown[5];
    for (int i = 0; i < 4; i++) {
        const uint32_t s = (o.updown >> (4 * i)) & 0xf;  // led.c: up2 is the lowest nibble
        updown[i] = s == 0 ? '0' : (s == 0xf ? '2' : '1');
    }
    updown[4] = 0;
    const int mode = o.midi_mode >= 0 && o.midi_mode <= 2 ? o.midi_mode : 0;
    chprintf(out, "Y %lu voice=%d midi=%s ch=%d voices=%d bend=%d tuning=%d fine=%s transpose=%s "
                  "pres=%s velo=%s tilt=%s bendsens=%s thres=%s vol=%d sustain=%s rate=%d alt=%d porta=%d "
                  "flip=%d led=%02x%02x%02x updown=%s\r\n",
             (unsigned long)counter, (int)o.voice, modes[mode], (int)o.channel_offset + 1, (int)o.voicecount,
             (int)(o.bend_range + 0.5f), (int)o.tuning, fine, transpose, pres, velo, tilt, bend, thres,
             (int)(o.volume + 0.5f), sustain, (int)o.message_interval, (int)o.alt, o.portamento ? 1 : 0,
             o.flipdir < 0 ? 1 : 0, rgb[0], rgb[1], rgb[2], updown);
}

// the general settings at boot, out of line: main calls synth_control_init, and main's stack is
// 1 kB (load_preset's deepest path runs on it)
__attribute__((noinline)) static int boot_preset(void) {
    const std::optional<int> i = settings::board().get_int(settings::KeyName::general("preset"));
    return i && *i >= 1 && *i <= 8 ? *i : 0;
}

__attribute__((noinline)) static void boot_settings(void) {
    using settings::KeyName;
    const settings::ConfigTable cfg = settings::board();
    if (auto i = cfg.get_int(KeyName::general("oct")); i && *i >= -2 && *i <= 2) {
        dis.change_note_offset(*i * dis.notegen0);
    }

    // deprecated, keep loading if setting still exists
    if (auto i = cfg.get_int(KeyName::general("motion")); i && *i >= 0 && *i <= 127) {
        config.send_motion_interval = *i;
    }

    if (cfg.is(KeyName::general("altkey"), "pedal")) {
        config.altkey_pedal = true;
    }
    if (cfg.is(KeyName::general("keyord"), "swap")) {
        config.key_swap = true;
    }
}

void synth_control_init(void) {
    // Load general settings. Use default settings when portamento key is pressed on boot
    if (palReadLine(LINE_BUTTON_PORT)) {
        if (const int preset = boot_preset()) {
            load_preset(preset);
        }
        boot_settings();
    }
}

void synth_tick(void) {
    dis.tick();
    if (dis.synth_in.fading()) {
        (void)messaging::send(Source::internal, IDI_MIDI_IN_TICK);  // (a full queue: the next tick)
    }
    if (dis.ump_offs_waiting) {
        (void)messaging::send(Source::internal, IDI_UMP_OUT_RETRY);  // MIDI 2.0 out's Note Offs waiting
    }
    // system reset (settings + key 67) waits for the settings button to be released: reset while it's
    // held and the UF2 bootloader sees its button and stays in update mode
    if (reset_on_release && !(dis.altmode & 1)) {
        chThdSleepMilliseconds(200);
        NVIC_SystemReset();
    }
}

// the indicator and the up/down leds as the instrument's state shows them (led_view.hpp), unless
// Settings is held and a key shows its value
void update_leds(void) {
    if (led_view::settings_shown(dis.altmode)) return;
    const int m = led_view::brightness(dis.altmode, dis.portamento);
    led_rgb3(led_view::indicator(dis.tuning_color, 16, m), led_view::indicator(dis.tuning_color, 8, m),
             led_view::indicator(dis.tuning_color, 0, m));
    led_updown(led_view::octave_leds(dis.flipdir, dis.start_note_offset));
}
