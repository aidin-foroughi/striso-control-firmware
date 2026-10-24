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
    #include "messaging.h"
}

#include "config.h"
#include "config_store.h"
#include "striso.h"
#include "midi_usb.h"
#include "midi_serial.h"
#include "midi.h"
#include "tuning.h"
#include "chprintf.h"

#include <bit>
#include <cstdint>
#include <type_traits>

#ifndef USE_WS2812
#define ws2812_write_led(n,r,g,b) led_rgb3(14*r,14*g,14*b)
#endif

// Behaviour changes a player would notice, off by default (the fix of a review finding that also
// changes how the instrument plays; research/review_synth_control.md). Build with -D<flag>=1.
// Normal mode's pitch bend in the same units as MPE's: bend_sensitivity semitones at full x. Off:
// twice that (Normal mode's bend range is 2 semitones and the bend went 0..1 of it) (SC-08).
#ifndef SYNTHCTL_NORMAL_BEND_LIKE_MPE
#define SYNTHCTL_NORMAL_BEND_LIKE_MPE 0
#endif

#define BUTTONCOUNT 68
#define MAX_PORTAMENTO_BUTTONS 8
static constexpr uint8_t MIDI_C_ALL_SOUND_OFF = 0x78;  // (midi.h has All Notes Off, 0x7b)

#define VOL_TICK (0.0005) // (1.0 / (SAMPLINGFREQ / CHANNEL_BUFFER_SIZE) / 0.5) // decay time of estimated volume
#define VOL_TICK_FACT (0.998) // 0.5**(1/(SAMPLINGFREQ / CHANNEL_BUFFER_SIZE)/0.1)

float volume_linear = 90.0f; // volume in range 0-127
void set_volume(float vol) {
    volume_linear = vol;
    volume = volume_linear * volume_linear * (1.0f / 16129.0f);
}

void MidiSend1(uint8_t b0) {
    midi_usb_MidiSend1(1, b0);
#ifdef USE_MIDI_SERIAL
    serial_MidiSend1(b0);
#endif
}

// (data bytes are 7 bit: a byte with bit 7 set would be read as a status byte)
void MidiSend2(uint8_t b0, uint8_t b1) {
    b1 &= 0x7f;
    midi_usb_MidiSend2(1, b0, b1);
#ifdef USE_MIDI_SERIAL
    serial_MidiSend2(b0, b1);
#endif
}

void MidiSend3(uint8_t b0, uint8_t b1, uint8_t b2) {
    b1 &= 0x7f;
    b2 &= 0x7f;
    midi_usb_MidiSend3(1, b0, b1, b2);
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

static inline uint32_t log2i(const uint32_t x){return (31 - __builtin_clz (x));}

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
void set_midi_mode(midi_mode_t mode);
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
                    d = ((((last_acc_x-64)<<6) > acc_x)<<4)-(1<<3);
                    acc_x = __USAT(64+((acc_x+(1<<5)+d)>>5), 7)&0x7F;
                    d = ((((last_acc_y-64)<<6) > acc_y)<<4)-(1<<3);
                    acc_y = __USAT(64+((acc_y+(1<<5)+d)>>5), 7)&0x7F;
                    d = ((((last_acc_z-64)<<6) > acc_z)<<4)-(1<<3);
                    acc_z = __USAT(64+((acc_z+(1<<5)+d)>>5), 7)&0x7F;
                    d = (((last_acc_abs<<5) > acc_abs)<<3)-(1<<2);
                    acc_abs = __USAT((acc_abs+(1<<4)+d)>>4, 7)&0x7F;
                    d = ((((last_rot_x-64)<<6) > rot_x)<<5)-(1<<4);
                    rot_x = __USAT(64+((rot_x+(1<<5)+d)>>6), 7)&0x7F;
                    d = ((((last_rot_y-64)<<6) > rot_y)<<5)-(1<<4);
                    rot_y = __USAT(64+((rot_y+(1<<5)+d)>>6), 7)&0x7F;
                    d = ((((last_rot_z-64)<<6) > rot_z)<<5)-(1<<4);
                    rot_z = __USAT(64+((rot_z+(1<<5)+d)>>6), 7)&0x7F;
                    if (acc_x != last_acc_x) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           16, acc_x);
                        last_acc_x = acc_x;
                    }
                    if (acc_y != last_acc_y) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           17, acc_y);
                        last_acc_y = acc_y;
                    }
                    if (acc_z != last_acc_z) {
                        MidiSend3(MIDI_CONTROL_CHANGE,
                                           18, acc_z);
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
        float synth_vol = 0.0;  // the same estimate, for the internal synth voices (tick)
        button_state_t state = STATE_OFF;
        int voice = -1;        // MIDI voice: the MPE channel slot (index into voices[])
        int synth_voice = -1;  // internal synth voice (index into synth_voices[]), -1: none
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
            if (synth_vol < vol0)
                synth_vol = vol0;
        }
};

class Instrument {
    public:
        Button buttons[BUTTONCOUNT];
        int voices[MAX_VOICECOUNT];
        int synth_voices[VOICECOUNT];  // the key of each internal synth voice, -1: none yet
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
            for (n = 0; n < VOICECOUNT; n++) {
                synth_voices[n] = -1;
            }
            for (n = 0; n < MAX_PORTAMENTO_BUTTONS; n++) {
                portamento_buttons[n] = -1;
            }
            synth_interface = si;
            set_volume(volume_linear);
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
                silence_synth_voice(b);
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
            for (int v = 0; v < VOICECOUNT; v++) {
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
            if (notegen1 < 6.94)       {tuning_color = 0x550055;}
            else if (notegen1 < 6.957) {tuning_color = 0x0000aa;}
            else if (notegen1 < 6.984) {tuning_color = 0x005555;}
            else if (notegen1 < 7.010) {tuning_color = 0x00aa00;}
            else if (notegen1 < 7.03)  {tuning_color = 0x555500;}
            else                       {tuning_color = 0xaa0000;}

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

        void load_tuning(int n) {
            if (cur_tuning == n) {
                led_rgb(tuning_color);
                return;
            }
            if (n == 0) {
                // tuning 0 hard coded to 12tet
                notegen0 = 12.0f;
                set_notegen1(7.0f);
                reset_note_offsets();
                note_offset = tuning_note_offset = 0.0f;
                led_rgb(tuning_color);
                cur_tuning = n;
                return;
            }
            float f;
            CC_ALIGN(8) char key[] = "fT0fifth";
            key[2] = '0' + n;
            f = getConfigFloat(key);
            if (f == CONFIG_UNDEFINED) f = 700.0f;
            notegen1 = f / 100;
            strset(key, 3, "oct  ");
            f = getConfigFloat(key);
            if (f == CONFIG_UNDEFINED) f = 1200.0f;
            notegen0 = f / 100;
            strset(key, 3, "off  ");
            f = getConfigFloat(key);
            if (f == CONFIG_UNDEFINED) f = 0.0f;
            note_offset = tuning_note_offset = f / 100;
            for (int n = 0; n < 61; n++) {
                put_button_name(n, &key[3]);
                int but = button_number_map[n];
                f = getConfigFloat(key);
                if (f == CONFIG_UNDEFINED) f = 0.0f;
                buttons[but].tuning_note_offset = f / 100;
            }
            key[0] = 'h';
            strset(key, 3, "color");
            tuning_color = getConfigHex(key);
            led_rgb(tuning_color);
            cur_tuning = n;
        }

        /* Rotate the layout 180 degrees */
        void flip(void) {
            flipdir = -flipdir;
            for (int n = 0; n < BUTTONCOUNT; n++) {
                buttons[n].coord0 = -buttons[n].coord0;
                buttons[n].coord1 = -buttons[n].coord1;
                // invert midi note number
                buttons[n].midinote_base = -buttons[n].midinote_base;
            }
        }

        void button_message(int but, float* msg) {
            static float old_angle = -1000.0f;
            static bool nudged = false;
            static systime_t next_knobchange;
            // process button message and send osc messages
            buttons[but].message(msg);

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
                            x = pow3(x) * bend_sensitivity;
#if SYNTHCTL_NORMAL_BEND_LIKE_MPE
                            x *= 0.5f;  // semitones, of Normal mode's bend range of 2 (midi_config)
#endif
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
                            change_note_offset(buttons[transpose_button].note
                                               - buttons[transpose_button2].note);
                            buttons[transpose_button2].midinote = buttons[transpose_button].midinote;
                            buttons[transpose_button2].voice = buttons[transpose_button].voice;
                            buttons[transpose_button2].midi_channel = buttons[transpose_button].midi_channel;
                            buttons[transpose_button2].new_channel_state();
                            buttons[transpose_button2].start_note_offset = start_note_offset;
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
        void take_synth_voice(int but, int v) {
            int old = synth_voices[v];
            if (old >= 0 && old != but) {
                buttons[old].synth_voice = -1;  // one key drives a voice
            }
            int mine = buttons[but].synth_voice;
            if (mine >= 0 && mine != v && synth_voices[mine] == but) {
                synth_voices[mine] = -1;
            }
            synth_voices[v] = but;
            buttons[but].synth_voice = v;
        }

        // the internal voice for a new note of but: its own voice (a repeated note), else an empty
        // one, else the quietest released one (by the estimated volume), else the quietest held
        // one if it is quieter than the new note (that key goes on on MIDI only); -1 if none
        int get_synth_voice(int but) {
#ifdef USE_INTERNAL_SYNTH
            int voice = -1;
            if (config.midi_mode == MIDI_MODE_MONO) {
                if (buttons[but].voice >= 0 && buttons[but].voice < VOICECOUNT) {
                    voice = buttons[but].voice;
                }
            } else if (buttons[but].synth_voice >= 0 && synth_voices[buttons[but].synth_voice] == but) {
                voice = buttons[but].synth_voice;
            } else {
                int released = -1, held = -1;
                float min_released = 0, min_held = buttons[but].synth_vol;
                for (int n = 0; n < VOICECOUNT; n++) {
                    if (synth_voices[n] == -1) {
                        voice = n;
                        break;
                    }
                    const Button& b = buttons[synth_voices[n]];
                    if (b.state != STATE_ON) {
                        if (released < 0 || b.synth_vol < min_released) {
                            min_released = b.synth_vol;
                            released = n;
                        }
                    } else if (b.synth_vol < min_held) {
                        min_held = b.synth_vol;
                        held = n;
                    }
                }
                if (voice < 0) {
                    voice = released >= 0 ? released : held;
                }
            }
            if (voice >= 0) {
                take_synth_voice(but, voice);
            }
            return voice;
#else
            (void)but;
            return -1;
#endif
        }

        // the note of but goes on with key to (portamento and free transpose hand-overs)
        void hand_over_synth_voice(int but, int to) {
            int v = buttons[but].synth_voice;
            if (v >= 0 && synth_voices[v] == but) {
                take_synth_voice(to, v);
            }
        }

        // the internal synth controls of but's voice (if it still has one)
        void update_synth_voice(int but) {
#ifdef USE_INTERNAL_SYNTH
            int voice = buttons[but].synth_voice;
            if (voice < 0 || voice >= VOICECOUNT || synth_voices[voice] != but) {
                return;
            }
            float pb = 0.0f;
            if (config.mpe_x == CFG_PITCH_BEND) {
                pb = bend_sensitivity * pow3(buttons[but].but_x);
            }
            *(synth_interface->note[voice])  = buttons[but].note + pb;
            *(synth_interface->pres[voice])  = buttons[but].pres * pres_sensitivity;
            *(synth_interface->vpres[voice]) = buttons[but].vpres * velo_sensitivity;
            *(synth_interface->but_x[voice]) = buttons[but].but_x;
            *(synth_interface->but_y[voice]) = clamp(buttons[but].but_y * y_sensitivity, -1.0f, 1.0f);
#else
            (void)but;
#endif
        }

        // end the internal sound of but's note (its key is still held)
        void silence_synth_voice(int but) {
#ifdef USE_INTERNAL_SYNTH
            int voice = buttons[but].synth_voice;
            if (voice >= 0 && voice < VOICECOUNT && synth_voices[voice] == but) {
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
                        silence_synth_voice(voices[voice]);  // a held note stolen
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
            for (int n = 0; n < VOICECOUNT; n++) {
                if (synth_voices[n] >= 0) {
                    Button& b = buttons[synth_voices[n]];
                    b.synth_vol = b.synth_vol * VOL_TICK_FACT - VOL_TICK;
                    if (b.synth_vol < b.vol0)
                        b.synth_vol = b.vol0;
                }
            }
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
 * (synth_message, ThreadSend), so it never races a real key press.
 * Allowed ids (config_but case numbers, current layout; every other id is refused):
 *    0  2  4                      MIDI mode MPE, Normal, Mono
 *   34                            tuning 12-TET
 *   36 38 23 25 27 29 31 33       tunings 1..8 (fT<n>... in the config)
 *   35 37 39 41 43 45 30 32       settings presets 1..8 (load_preset: MIDI mode, sensitivities...)
 *   18 20 22 24 26 28 13          voices strisy, va, modal, bowed, wind, ep, brass (internal synth)
 *   63                            panic: all notes off (MIDI)
 * Refused: 67 system reset, 65 send MPE/bend config, 11 debug/flip layout, the dials (17 threshold,
 * 19 pressure, 21 velocity, 6 bend, 8 tilt, 51 volume, 53 sustain, 56 MPE bend range, 58 message
 * interval: they need an amount, not a press) and anything else. None of the allowed keys writes
 * flash or reboots.
 */
static bool settings_key_allowed(int id) {
#ifdef OLD_CONFIG_LAYOUT
    (void)id;
    return false;
#else
    switch (id) {
    case 0: case 2: case 4:                                                  // MIDI modes
    case 34: case 36: case 38: case 23: case 25: case 27: case 29: case 31: case 33:  // tunings
    case 35: case 37: case 39: case 41: case 43: case 45: case 30: case 32:  // settings presets
#ifdef USE_INTERNAL_SYNTH
    case 18: case 20: case 22: case 24: case 26: case 28: case 13: case 15:  // voices
#endif
    case 63:                                                                 // panic
        return true;
    default:
        return false;
    }
#endif
}

static void settings_key_press(int id) {
    if (!settings_key_allowed(id)) return;
    config_but(id, 0, 0);
    update_leds();
}

int synth_settings_key(int id) {
    if (!settings_key_allowed(id)) return 0;
    int msg[3] = {ID_INTERNAL, IDI_SETTINGS_KEY, id};
    return msgSend(3, msg) == 0;  // 0: queued
}

int synth_message(int size, int* msg) {
    static float note_offset_old;
    float fmsg[7];
    int src = msg[0];
    int id = msg[1];
    msg = &msg[2];
    size -= 2;

    if (src == ID_INTERNAL) {  // requests queued by synth_settings_key() (console StcK)
        if (id == IDI_SETTINGS_KEY && size >= 1) settings_key_press(msg[0]);
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
            *(dis.synth_interface->pedal) = fmsg[0];
            pedal.send(msg[0]>>4, 0);//dis.midi_channel_offset);
        }
        else if (id == IDC_PEDAL_1) {
            // Switch pedal 1 (tip)
            int val = config.pedal_sw1_values[msg[0]>0];
            *(dis.synth_interface->pedal) = (float)val/127;
            pedal.send(val*4, 0);//dis.midi_channel_offset);
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
        }
    }
    else {
        // unknown message source
        return -1;
    }
    return 0;
}

void load_preset(int n) {
    int i;
    float f;
    const char* s;
    bool sendcfg = false;
    CC_ALIGN(8) char key[] = "hP0color";
    key[2] = '0' + n;
    uint32_t led = getConfigHex(key);
    led_rgb(led);

    key[0] = 'i';
    strset(key, 3, "Mint ");
    i = getConfigInt(key);
    if (i >= 1 && i <= 127) {
        config.message_interval = i;
    }

    strset(key, 3, "Mmint");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        config.send_motion_interval = i;
    }

    key[0] = 's';
    strset(key, 3, "Mmode");
    s = getConfigSetting(key);
    if (cmp8(s, "mpe     ")) {
        set_midi_mode(MIDI_MODE_MPE);
        sendcfg = true;
    } else if (cmp8(s, "normal  ")) {
        set_midi_mode(MIDI_MODE_POLY);
        sendcfg = true;
    } else if (cmp8(s, "mono    ")) {
        set_midi_mode(MIDI_MODE_MONO);
        sendcfg = true;
    }

    strset(key, 3, "Mnote");
    s = getConfigSetting(key);
    if (cmp8(s, "default ")) {
        config.midinote_mode = MIDINOTE_MODE_DEFAULT;
    } else if (cmp8(s, "tuning  ")) {
        config.midinote_mode = MIDINOTE_MODE_TUNING;
    } else if (cmp8(s, "button  ")) {
        config.midinote_mode = MIDINOTE_MODE_BUTTON;
    }

    strset(key, 3, "jack2");
    s = getConfigSetting(key);
    if (cmp8(s, "pedal_ex")) {
        aux_jack_switch_mode_setting(JACK2_MODE_PEDAL_EXPRESSION);
    } else if (cmp8(s, "pedal_sw")) {
        aux_jack_switch_mode_setting(JACK2_MODE_PEDAL_SWITCH);
    } else if (cmp8(s, "midi    ")) {
        aux_jack_switch_mode_setting(JACK2_MODE_MIDI);
    } else if (cmp8(s, "linein  ")) {
        aux_jack_switch_mode_setting(JACK2_MODE_LINEIN);
    } else if (cmp8(s, "auto    ")) {
        aux_jack_switch_mode_setting(JACK2_MODE_AUTODETECT);
    }

    strset(key, 3, "flip ");
    s = getConfigSetting(key);
    if ((cmp8(s, "yes     ") && dis.flipdir == 1)
        || (cmp8(s, "no      ") && dis.flipdir == -1)) {
        dis.flip();
    }

    key[0] = 'i';
    strset(key, 3, "tunin");
    i = getConfigInt(key);
    if (i >= 0 && i <= 8) {
        dis.load_tuning(i);
    }

    key[0] = 'f';
    strset(key, 3, "Toff ");
    f = getConfigFloat(key);
    if (f >= -4800.0f && f <= 4800.0f) {
        dis.note_offset = dis.tuning_note_offset + f / 100.0f;
    }

    key[0] = 'i';
    strset(key, 3, "Mpres");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        if (config.midi_mode == MIDI_MODE_POLY) {
            config.midi_pres = i;
        } else {
            config.mpe_pres = i;
        }
    }

    strset(key, 3, "Mx   ");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        if (config.midi_mode == MIDI_MODE_POLY) {
            config.midi_x = i;
        } else {
            config.mpe_x = i;
        }
    }

    strset(key, 3, "My   ");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        if (config.midi_mode == MIDI_MODE_POLY) {
            config.midi_y = i;
        } else {
            config.mpe_y = i;
        }
    }

    strset(key, 3, "Mvelo");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        config.mpe_contvelo = i;
    }

    strset(key, 3, "MChan");
    i = getConfigInt(key);
    if (i >= 1 && i <= 16) {
        dis.midi_channel_offset = i - 1;
        sendcfg = true;
    }

    strset(key, 3, "MPEpb");
    i = getConfigInt(key);
    if (i >= 1 && i <= 127) {
        dis.midi_bend_range = i;
        sendcfg = true;
    }

    strset(key, 3, "voice");
    i = getConfigInt(key);
    if (i >= 1 && i <= MAX_VOICECOUNT && config.midi_mode == MIDI_MODE_MPE) {
        dis.voicecount = i;
        sendcfg = true;
    }
    dis.channels_changed();

    key[0] = 'f';
    strset(key, 3, "thres");
    f = getConfigFloat(key);
    if (f >= 0.0f && f <= 1.0f) {
        config.zero_offset = clamp(f, 0.0f, 0.5f) * (1 << 24);
    }

    strset(key, 3, "bendS");
    f = getConfigFloat(key);
    if (f >= -10.0f && f <= 10.0f) {
        dis.bend_sensitivity = f * 2;
    }

    strset(key, 3, "presS");
    f = getConfigFloat(key);
    if (f >= 0.0f && f <= 10.0f) {
        dis.pres_sensitivity = f;
    }

    strset(key, 3, "veloS");
    f = getConfigFloat(key);
    if (f >= 0.0f && f <= 10.0f) {
        dis.velo_sensitivity = f;
    }

    strset(key, 3, "tiltS");
    f = getConfigFloat(key);
    if (f >= -10.0f && f <= 10.0f) {
        dis.y_sensitivity = f;
    }

    strset(key, 3, "volum");
    f = getConfigFloat(key);
    if (f >= 0.0f && f < 255.0f) {
        set_volume(f);
    }

    strset(key, 3, "decay");
    f = getConfigFloat(key);
    if (f >= 0.0f && f < 255.0f) {
        config.pedal_sw1_values[0] = f;
        *(dis.synth_interface->pedal) = f * (1.0f/127.0f);
    }

    strset(key, 3, "decaP");
    f = getConfigFloat(key);
    if (f >= 0.0f && f < 255.0f) {
        config.pedal_sw1_values[1] = f;
    }

    key[0] = 'i';

    strset(key, 3, "veloO");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        dis.midi_velo_offset = i;
    }

    if (sendcfg) {
        midi_config();
    }

    strset(key, 3, "Mpgm ");
    i = getConfigInt(key);
    if (i >= 0 && i <= 127) {
        if (config.midi_mode != MIDI_MODE_POLY && dis.midi_channel_offset == 1) {
            MidiSend2(MIDI_PROGRAM_CHANGE, i);
        }
        for (int n = 0; n < dis.voicecount; n++) {
            MidiSend2(MIDI_PROGRAM_CHANGE | dis.voice_channel(n), i);
        }
    }

    led_rgb(led);
}

#ifdef OLD_CONFIG_LAYOUT
float config_but(int but, int type, float adjust) {
    if (type > 0) return 0;
    switch (but) {
        // row 1:  0  2  4
        case (0): {
            set_midi_mode(MIDI_MODE_MPE);
        } return 0;
        case (2): {
            set_midi_mode(MIDI_MODE_POLY);
        } return 0;
        case (4): {
            set_midi_mode(MIDI_MODE_MONO);
        } return 0;
        // row 2:  1  3  5  7  9 11
        case (1): {
            config.message_interval = 1;
            ws2812_write_led(0, 1, 12, 0);
        } return 0;
        case (3): {
            config.message_interval = 10;
            ws2812_write_led(0, 4, 2, 0);
        } return 0;
        case (5): {
            config.send_motion_interval = 1;
            ws2812_write_led(0, 1, 12, 0);
        } return 0;
        case (7): {
            config.send_motion_interval = 10;
            ws2812_write_led(0, 4, 2, 0);
        } return 0;
        case (9): {
            config.send_motion_interval = 0;
            *(dis.synth_interface->acc_abs) = 1.0f;
            *(dis.synth_interface->acc_x) = 0.0f;
            *(dis.synth_interface->acc_y) = 0.0f;
            *(dis.synth_interface->acc_z) = 0.0f;
            *(dis.synth_interface->rot_x) = 0.0f;
            *(dis.synth_interface->rot_y) = 0.0f;
            *(dis.synth_interface->rot_z) = 0.0f;
            ws2812_write_led(0, 3, 0, 0);
        } return 0;
        case (11): { // debug setting for easy testing
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
        // row 3: 17 19 21  6  8 10 12 14 16
        case (17): {
            dis.pres_sensitivity = 0.67f;
            ws2812_write_led(0, 1, 1, 0);
        } return 0;
        case (19): {
            dis.pres_sensitivity = 1.0f;
            ws2812_write_led(0, 4, 4, 0);
        } return 0;
        case (21): {
            dis.pres_sensitivity = 1.5f;
            ws2812_write_led(0, 12, 12, 0);
        } return 0;
        case (6): {
            dis.velo_sensitivity = 0.67f;
            ws2812_write_led(0, 1, 0, 1);
        } return 0;
        case (8): {
            dis.velo_sensitivity = 1.0f;
            ws2812_write_led(0, 4, 0, 4);
        } return 0;
        case (10): {
            dis.velo_sensitivity = 1.5f;
            ws2812_write_led(0, 12, 0, 12);
        } return 0;
        case (12): {
            dis.bend_sensitivity = 0.0f;
            ws2812_write_led(0, 1, 0, 0);
        } return 0;
        case (14): {
            dis.bend_sensitivity = 0.25f;
            ws2812_write_led(0, 4, 0, 0);
        } return 0;
        case (16): {
            dis.bend_sensitivity = 0.5f;
            ws2812_write_led(0, 12, 0, 0);
        } return 0;
        // row 4: 18 20 22 24 26 28 13 15
        case (18): {
            config.zero_offset = 0 * ((1<<24) / 128);
            led_rgb3(14, 7, 0);
        } return 0;
        case (20): {
            config.zero_offset = 4 * ((1<<24) / 128);
            led_rgb3(56, 28, 0);
        } return 0;
        case (22): {
            config.zero_offset = 8 * ((1<<24) / 128);
            led_rgb3(168, 84, 0);
        } return 0;
        // Load config preset
        // row 5: 34 36 38 23 25 27 29 31 33
        case (34): { // set 12tet tuning
            dis.load_tuning(0);
        } return 0;
        case (36): {
            dis.load_tuning(1);
        } return 0;
        case (38): {
            dis.load_tuning(2);
        } return 0;
        case (23): {
            dis.load_tuning(3);
        } return 0;
        case (25): {
            dis.load_tuning(4);
        } return 0;
        case (27): {
            dis.load_tuning(5);
        } return 0;
        case (29): {
            dis.load_tuning(6);
        } return 0;
        case (31): {
            dis.load_tuning(7);
        } return 0;
        case (33): {
            dis.load_tuning(8);
        } return 0;
        // row 6: 35 37 39 41 43 45 30 32
        case (35): {
            load_preset(1);
        } return 0;
        case (37): {
            load_preset(2);
        } return 0;
        case (39): {
            load_preset(3);
        } return 0;
        case (41): {
            load_preset(4);
        } return 0;
        case (43): {
            load_preset(5);
        } return 0;
        case (45): {
            load_preset(6);
        } return 0;
        case (30): {
            load_preset(7);
        } return 0;
        case (32): {
            load_preset(8);
        } return 0;
        // row 7: 51 53 55 40 42 44 46 48 50
        case (51): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 7);
            set_volume(7);
            ws2812_write_led(0, 1, 1, 1);
        } return 0;
        case (53): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 15);
            set_volume(15);
            ws2812_write_led(0, 2, 2, 2);
        } return 0;
        case (55): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 31);
            set_volume(31);
            ws2812_write_led(0, 3, 3, 3);
        } return 0;
        case (40): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 47);
            set_volume(47);
            ws2812_write_led(0, 4, 4, 4);
        } return 0;
        case (42): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 63);
            set_volume(63);
            ws2812_write_led(0, 6, 6, 6);
        } return 0;
        case (44): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 79);
            set_volume(79);
            ws2812_write_led(0, 8, 8, 8);
        } return 0;
        case (46): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 95);
            set_volume(95);
            ws2812_write_led(0, 10, 10, 10);
        } return 0;
        case (48): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 111);
            set_volume(111);
            ws2812_write_led(0, 14, 14, 14);
        } return 0;
        case (50): {
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_MAIN_VOLUME, 127);
            set_volume(127);
            ws2812_write_led(0, 18, 18, 18);
        } return 0;

        // row 8:       56 58 60 62 47 49
        // row 9:                   63 65 67
        case (63): { // panic/request config
#ifdef USE_MIDI_OUT
            MidiSend3(MIDI_CONTROL_CHANGE,
                                MIDI_C_ALL_NOTES_OFF, 0);
            ws2812_write_led(0, 16, 0, 0);
#endif
        } return 0;
        case (65): { // send config
#ifdef USE_MIDI_OUT
            // TODO: send config
            ws2812_write_led(0, 16, 0, 0);
#endif
        } return 0;
        case (67): { // system reset
            led_rgb(0xff0000);
            reset_on_release = true;
        } return 0;
        default: return 0;
    }
}
#else // !OLD_CONFIG_LAYOUT

#ifdef USE_INTERNAL_SYNTH
/*
 * Synth voice selection with the settings row 4 ("future use" on the v2.2 settings card): each key
 * picks a voice engine, shown by the indicator led colour. On an engine with presets (the VA) the
 * key works like the other value keys: nudge it or circle it to step through the presets, the
 * up/down leds show which one. The last preset of each engine is remembered while powered.
 * Returns the unused part of adjust, like config_but.
 */
static float config_voice(const char* engine, int r, int g, int b, int type, float adjust) {
    static int8_t remembered[64];  // preset per engine, indexed by the engine's first entry
    int first = -1, count = 0;
    for (int n = 0; n < synth_num_voices(); n++) {
        const char* preset;
        if (strcmp(synth_voice_name(n, &preset), engine) == 0) {
            if (first < 0) first = n;
            count++;
        }
    }
    if (count == 0 || first >= 64) {
        led_rgb3(64, 0, 0);  // engine not in this firmware
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
    led_updown_dial(count > 1 ? idx + 1 : 0);
    return type > 0 ? adjust - a : 0;
}
#endif

/*
 * button functions in alt mode - with settings button pressed.
 *
 * type can be 0: initial press, 1: nudge up/down, 2: circular adjust
 *
 * returns part of adjust that's not used and hence needs to be saved for next call
 */
float config_but(int but, int type, float adjust) {
    switch (but) {
    // row 1:  0  2  4
    case (0): // MPE MIDI mode, knob: number of MPE channels
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
    case (2): // Normal MIDI mode
        if (type == 0) {
            if (config.midi_mode != MIDI_MODE_POLY) set_midi_mode(MIDI_MODE_POLY);
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
    case (4): // Mono (glissando) MIDI mode
        if (type == 0) {
            if (config.midi_mode != MIDI_MODE_MONO) set_midi_mode(MIDI_MODE_MONO);
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
    // row 2:  1  3  5  7  9 11
    case (11): // debug setting for easy testing
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
    // row 3: 17 19 21  6  8 10 12 14 16
    case (17): { // knob: key detection threshold
        int a = adjust * ((1 << 24) / 256);
        if (type > 0) {
            config.zero_offset = clamp(config.zero_offset + a, 0, 16 * (1 << 24) / 256 - 1);
        }
        int dial = clamp(config.zero_offset / ((1 << 24) / 256), 0, 15);
        led_updown_dial(dial);
        led_rgb3(dial * 10, dial * 5, 0);
        return 0; // TODO: sticky ends
        }
    case (19): { // knob: pres sensitivity
        float rem = 0;
        if (type > 0) {
            dis.pres_sensitivity = clamp_rem(dis.pres_sensitivity + adjust * (1.0f/8.0f), 0, 3.0f, &rem);
        }
        led_updown_dial(dis.pres_sensitivity * 8 + 0.9f);
        led_rgb3(dis.pres_sensitivity * 64.0f, dis.pres_sensitivity * 64.0f, 0);
        return rem * 8;
        }
    case (21): { // knob: velo sensitivity
        float rem = 0;
        if (type > 0) {
            dis.velo_sensitivity = clamp_rem(dis.velo_sensitivity + adjust * (1.0f/8.0f), 0, 2.0f, &rem);
        }
        led_updown_dial(min((int)(dis.velo_sensitivity * 8 + 0.9f), 15));
        led_rgb3(dis.velo_sensitivity * 64.0f, 0, dis.velo_sensitivity * 64.0f);
        return rem * 8;
        }
    case (6): { // knob: pitchbend range
        float rem = 0;
        if (type > 0) {
            dis.bend_sensitivity = clamp_rem(dis.bend_sensitivity + adjust * 0.25f, 0, 4.0f, &rem);
        }
        led_updown_dial(min((int)(dis.bend_sensitivity * 4 + 0.9f), 15));
        led_rgb3(dis.bend_sensitivity * 64.0f, 0, 0);
        return rem * 4;
        }
    case (8): { // knob: y sensitivity (TODO: flip?)
        float rem = 0;
        if (type > 0) {
            dis.y_sensitivity = clamp_rem(dis.y_sensitivity + adjust * (1.0f/8.0f), 0, 3.0f, &rem);
        }
        led_updown_dial(dis.y_sensitivity * 8 + 0.5f);
        led_rgb3(dis.y_sensitivity * 32.0f, 0, dis.y_sensitivity * 64.0f);
        return rem * 8;
        }
    // row 4: 18 20 22 24 26 28 13 15
#ifdef USE_INTERNAL_SYNTH
    // row 4: 18 20 22 24 26 28 13 15 ("future use" on the v2.2 card): synth voice
    case (18): return config_voice("strisy", 40, 40, 40, type, adjust);
    case (20): return config_voice("va", 64, 20, 0, type, adjust);
    case (22): return config_voice("modal", 0, 24, 64, type, adjust);
    case (24): return config_voice("bowed", 64, 0, 0, type, adjust);
    case (26): return config_voice("wind", 0, 64, 0, type, adjust);
    case (28): return config_voice("ep", 40, 0, 64, type, adjust);
    case (13): return config_voice("brass", 64, 40, 0, type, adjust);
    case (15): return config_voice("trumpet", 0, 48, 48, type, adjust);  // cyan
#endif
    // row 5: 34 36 38 23 25 27 29 31 33
    case (34): // set 12tet tuning, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(0);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (36): // load tuning 1, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(1);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (38): // load tuning 2, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(2);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (23): // load tuning 3, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(3);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (25): // load tuning 4, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(4);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (27): // load tuning 5, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(5);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (29): // load tuning 6, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(6);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (31): // load tuning 7, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(7);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
    case (33): // load tuning 8, knob: tuning offset
        if (type == 0) {
            dis.load_tuning(8);
        } else {
            dis.note_offset += adjust * (1.0f/16.0f/2.0f);
        }
        led_updown_dial(dis.note_offset * 16 + 0.5f);
        return 0;
        // row 6: 35 37 39 41 43 45 30 32
    case (35):
        if (type == 0) {
            load_preset(1);
        } return 0;
    case (37):
        if (type == 0) {
            load_preset(2);
        } return 0;
    case (39):
        if (type == 0) {
            load_preset(3);
        } return 0;
    case (41):
        if (type == 0) {
            load_preset(4);
        } return 0;
    case (43):
        if (type == 0) {
            load_preset(5);
        } return 0;
    case (45):
        if (type == 0) {
            load_preset(6);
        } return 0;
    case (30):
        if (type == 0) {
            load_preset(7);
        } return 0;
    case (32):
        if (type == 0) {
            load_preset(8);
        } return 0;
    // row 7: 51 53 55 40 42 44 46 48 50
    case (51): { // knob: Volume (CC7)
        float rem = 0;
        if (type > 0) {
            float vol = clamp_rem(volume_linear + adjust * 8.0f, 0, 127.0f, &rem);
            if ((int)(volume_linear + 0.5f) != (int)(vol + 0.5f)) {
                MidiSend3(MIDI_CONTROL_CHANGE, MIDI_C_MAIN_VOLUME, vol + 0.5f);
            }
            set_volume(vol);
        }
        led_rgb3(volume_linear*2, volume_linear*2, volume_linear*2);
        led_updown_dial((int)volume_linear/8);
        return rem * (1.0f/8.0f);
        }
    case (53): { // knob: sustain/decay rate
        float rem = 0;
        if (type > 0) {
            *(dis.synth_interface->pedal) = clamp_rem(*(dis.synth_interface->pedal) + adjust * (1.0f/16.0f), 0, 1.0f, &rem);
            pedal.send(*(dis.synth_interface->pedal) * 128 * 4, 0);
        }
        led_updown_dial(*(dis.synth_interface->pedal) * 15 + 0.5f);
        led_rgb3(*(dis.synth_interface->pedal) * 64.0f, 0, *(dis.synth_interface->pedal) * 64.0f);
        return rem * 16;
        }
    // row 8:       56 58 60 62 47 49
    case (56): { // knob: MPE pitchbend range
        int a = adjust;
        int cur = log2i(dis.midi_bend_range / 12);
        if (type > 0) {
            if (type == 2) a = a / 4;
            if (a != 0) {
                cur = clamp(cur + a, 0, 3);
                dis.midi_bend_range = 12 << cur;
                midi_config();
            }
            if (type == 2) a = a * 4;
        }
        led_rgb3(10 + 50*cur, 0, 0);
        led_updown_dial(2 + 2 * cur);
        return adjust - a;
        }
    case (58): { // knob: MIDI message interval
        int a = (int)adjust;
        if (type > 0) {
            config.message_interval = clamp(config.message_interval - a, 1, 15);
        }
        led_rgb3(164-8*config.message_interval, 164-8*config.message_interval, 0);
        led_updown_dial(16 - config.message_interval);
        return adjust - a;
        }
    // case (60): { // knob: MIDI motion message interval
    //     int a = (int)adjust;
    //     if (type > 0) {
    //         config.send_motion_interval = clamp(config.send_motion_interval - adjust, 0, 15);
    //         if (angle == 0) {
    //             config.send_motion_interval = 0;
    //             *(dis.synth_interface->acc_abs) = 1.0f;
    //             *(dis.synth_interface->acc_x) = 0.0f;
    //             *(dis.synth_interface->acc_y) = 0.0f;
    //             *(dis.synth_interface->acc_z) = 0.0f;
    //             *(dis.synth_interface->rot_x) = 0.0f;
    //             *(dis.synth_interface->rot_y) = 0.0f;
    //             *(dis.synth_interface->rot_z) = 0.0f;
    //         } else if (angle == 15) {
    //             config.send_motion_interval = 127;
    //         } else {
    //             config.send_motion_interval = angle;
    //         }
    //     }
    //     led_rgb3(164-8*config.send_motion_interval, 164-8*config.send_motion_interval, 0);
    //     led_updown_dial(16 - config.send_motion_interval);
    //     return adjust - a;
    //     }
    // row 9:                   63 65 67
    case (63): // panic/all notes off
        if (type == 0) {
            dis.panic();
            led_rgb(0x342000);
        } return 0;
    case (65): // send MPE/pitch bend range config
        if (type == 0) {
            midi_config();
            led_rgb(0x300064);
        } return 0;
    case (67): // system reset
        if (type == 0) {
            led_rgb(0xff0000);
            reset_on_release = true;
        } return 0;
    default: return 0;
    }
    return 0;
}
#endif // !OLD_CONFIG_LAYOUT

void MidiInMsgHandler(midi_device_t dev, uint8_t port, uint8_t status,
                      uint8_t data1, uint8_t data2) {
    (void) dev;
    (void) port;
    // the selected RPN per channel, 0x7f/0x7f: none (as at power on, so a bare Data Entry does
    // nothing; RPN 0/0 is the pitch bend range). An NRPN select deselects the RPN.
    static uint8_t lastRPN_LSB[16] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
                                      0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
    static uint8_t lastRPN_MSB[16] = {0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f,
                                      0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f, 0x7f};
    static uint8_t lsb_cc1 = 0;

    uint8_t channel = status & 0x0f;
    status &= 0xf0;

    led_rgb3_blink(255, 0, 127, TIME_MS2I(100));

    if (config.send_midi_monitor) {
        // Monitor MIDI input
        int msg[5];
        msg[0] = ID_MIDI;
        msg[1] = channel;
        msg[2] = status >> 4;
        msg[3] = data1;
        msg[4] = data2;
        msgSend(5, msg);
    }

    if (status == MIDI_CONTROL_CHANGE) {
        switch (data1) {
            case MIDI_C_RPN_LSB: lastRPN_LSB[channel] = data2; break;
            case MIDI_C_RPN_MSB: lastRPN_MSB[channel] = data2; break;
            case MIDI_C_NONRPN_LSB:
            case MIDI_C_NONRPN_MSB: lastRPN_LSB[channel] = lastRPN_MSB[channel] = 0x7f; break;
            case MIDI_C_DATA_ENTRY: {
                const uint8_t lastRPN_LSB_ch = lastRPN_LSB[channel], lastRPN_MSB_ch = lastRPN_MSB[channel];
                if (lastRPN_LSB_ch == 0 && lastRPN_MSB_ch == 0) {
                    // pitch bend range of the notes: at least 1 (0 divides by zero). An MPE host
                    // sends one per channel; the master channel's (2) is not the notes' range.
                    bool mpe_master = config.midi_mode == MIDI_MODE_MPE && dis.midi_channel_offset == 1 && channel == 0;
                    if (data2 >= 1 && !mpe_master) {
                        dis.midi_bend_range = data2;
                    }
                } else if (lastRPN_LSB_ch == 6 && lastRPN_MSB_ch == 0) {
                    // MPE Configuration Message: the lower zone (master channel 1) with data2 member
                    // channels. 0 (zone off) is ignored rather than leaving no channel; the upper
                    // zone (master channel 16) isn't supported. The same configuration again (an
                    // echo of midi_config's) changes nothing.
                    if (channel == 0 && data2 >= 1 && data2 <= MAX_VOICECOUNT
                        && !(config.midi_mode == MIDI_MODE_MPE && dis.midi_channel_offset == 1 && dis.voicecount == data2)) {
                        set_midi_mode(MIDI_MODE_MPE);
                        dis.midi_channel_offset = 1;
                        dis.voicecount = data2;
                        dis.channels_changed();
                    }
                } else if (lastRPN_LSB_ch == 1 && lastRPN_MSB_ch == 0) {
                    // TODO: Tuning, done with pitch bend
                }
            } break;
            case  1: { // Fifth (microtonal) tuning
                dis.set_notegen1(6.80 + (float)data2 * (0.20/64) + (float)lsb_cc1 * (0.20/8192));
                update_leds();
            } break;
            case  1|MIDI_C_LSB: lsb_cc1 = data2; break;
            case  7: set_volume(data2); break;
            case 16: config.send_motion_interval = data2; break;
            // case 16|MIDI_C_LSB: config.send_motion_14bit = data2; break;
            case 17: config.message_interval = data2 >= 1 ? data2 : 1; break;
            // case 17|MIDI_C_LSB: config.send_button_14bit = data2; break;
            case 18: { // Note offset
                dis.set_note_offset(data2 - 2);
                update_leds();
            } break;
            case 64: {
                *(dis.synth_interface->pedal) = (float)data2 * (1.0f/127.0f);
            } break;
            case 65: {
                dis.set_portamento(data2);
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
            case 74: { // y
                if (data2 == 0) data2 = CFG_DISABLE;
                if (config.midi_mode == MIDI_MODE_MPE) {
                    config.mpe_y = data2;
                } else {
                    config.midi_y = data2;
                }
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
    } else if (status == MIDI_PITCH_BEND) {
        dis.note_offset = (float)((int)((data2<<7)+data1)-0x2000) * (2.0/8192);
    }
}

void set_midi_mode(midi_mode_t mode) {
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

void synth_control_init(void) {
    // Load general settings. Use default settings when portamento key is pressed on boot
    if (palReadLine(LINE_BUTTON_PORT)) {
        int i = getConfigInt("iGpreset");
        if (i >= 1 && i <= 8) {
            load_preset(i);
        }
        i = getConfigInt("iGoct   ");
        if (i >= -2 && i <= 2) {
            dis.change_note_offset(i * dis.notegen0);
        }

        // deprecated, keep loading if setting still exists
        i = getConfigInt("iGmotion");
        if (i >= 0 && i <= 127) {
            config.send_motion_interval = i;
        }

        const char* s = getConfigSetting("sGaltkey");
        if (cmp8(s, "pedal   ")) {
            config.altkey_pedal = true;
        }
        s = getConfigSetting("sGkeyord");
        if (cmp8(s, "swap    ")) {
            config.key_swap = true;
        }
    }
}

void synth_tick(void) {
    dis.tick();
    // system reset (settings + key 67) waits for the settings button to be released: reset while it's
    // held and the UF2 bootloader sees its button and stays in update mode
    if (reset_on_release && !(dis.altmode & 1)) {
        chThdSleepMilliseconds(200);
        NVIC_SystemReset();
    }
}

void update_leds(void) {
    // if buttons in alt mode don't update leds
    if (dis.altmode & 2) return;

    uint8_t r, g, b;
    r = dis.tuning_color >> 16 & 0xff;
    g = dis.tuning_color >>  8 & 0xff;
    b = dis.tuning_color >>  0 & 0xff;

    int m = 64 + 16 * (dis.altmode + (dis.portamento & 1));
    led_rgb3(r * m / 64, g * m / 64, b * m / 64);

    int note_offset = dis.flipdir * ((int)(dis.start_note_offset + 0.5f) - 62);
    if      (note_offset <= -24) led_updown(0xff00);
    else if (note_offset <  -12) led_updown(0xaf00);
    else if (note_offset <= -12) led_updown(0x0f00);
    else if (note_offset <    0) led_updown(0x0a00);
    else if (note_offset >=  24) led_updown(0x00ff);
    else if (note_offset >   12) led_updown(0x00fa);
    else if (note_offset >=  12) led_updown(0x00f0);
    else if (note_offset >    0) led_updown(0x00a0);
    else                         led_updown(0x0000);
}
