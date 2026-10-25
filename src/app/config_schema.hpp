// The configuration schema: every setting the firmware reads, its type, its default and what the
// config editor shows for it. The one source of:
//   - default_config, the firmware's default records (config_table.hpp builds them at compile time;
//     tests/golden/firmware_bytes.txt has their bytes),
//   - the config editor's key list (config_editor/config_editor.py generates its `doc` table from
//     this file at build time),
//   - tools/striso.py's record and key checks (tools/config_schema.py reads this file).
// tools/config_schema.py parses it, so keep its form: one entry per line, designated initializers,
// string literals without escapes, the tables in this order.
//
// The records (src/app/config_store.h): 16 bytes, an 8-character key and an 8-character value,
// both padded with spaces, from the header record to the end record. A key is
//   <type><group>[<number>]<name>[<key name>], padded to 8 characters:
//   type    s text, i integer, f decimal, h hexadecimal colour (#rrggbb)
//   group   G general, P preset (number 1-8), T tuning (number 1-8; tuning 0, 12-TET, is built
//           in), C calibration
//   key     for the per-key fields: the key's name, e.g. F_1 (put_button_name)
// e.g. iGpreset, iP1Mint, fT8fifth, fT8F_1, fCfacB_4.
// The user's settings are a page of these records in flash; a key there overrides its default.
#pragma once

// clang-format off
namespace config_schema {

// the records: the address of the user's page (the config sector, ld/STM32H743xI_uf2.ld), its
// size, and the header and end records (key, value)
inline constexpr unsigned kFlashAddress = 0x08020000;
inline constexpr unsigned kFlashSize = 0x20000;
inline constexpr const char* kHeaderKey = "MConfig ";
inline constexpr const char* kHeaderValue = "v1.0    ";
inline constexpr const char* kEndKey = "MCfgEnd ";
inline constexpr const char* kEndValue = "        ";

// the 61 keys' names: key number n (0..60) is note (n + 7) % 17 of these, octave (n + 7) / 17 + 1
// (F_1, Gb1, F#1, ... B_4)
inline constexpr int kKeys = 61;
inline constexpr const char* kNoteNames = "C_DbC#D_EbD#E_F_GbF#G_AbG#A_BbA#B_";

struct Field {
	char type;                 // 's' text, 'i' integer, 'f' decimal, 'h' hexadecimal
	const char* name;          // after the group and number: up to 6 characters (general), 5 (preset, tuning), 3 (per key)
	const char* value = "";    // the default (up to 8 characters; "": blank, the setting is not set)
	const char* text = "";     // config editor: what it is ("": no description)
	const char* help = "";     // config editor: its values
	const char* options = "";  // config editor: the choices, '|' separated; a leading '|' offers blank too
	double lo = 0, hi = -1;    // config editor: the value is clamped to [lo, hi] (none if lo > hi)
	bool per_key = false;      // one record per key, the key's name after the name
	bool deprecated = false;   // still read if the user's page has it; no default, the editor shows it only then
};

// general settings: <type>G<name>
inline constexpr Field kGeneral[] = {
	{.type = 'i', .name = "preset", .text = "Preset to load on start", .help = "[1-8]", .lo = 1, .hi = 8},
	{.type = 'i', .name = "oct", .value = "0", .text = "Default octave", .help = "[-2..2]", .lo = -2, .hi = 2},
	{.type = 's', .name = "altkey", .value = "settings", .text = "Settings key mode", .help = "[settings: Use key for settings (default), pedal: Disable settings and use key as pedal (for simplified use)]", .options = "settings|pedal"},
	{.type = 's', .name = "keyord", .value = "normal", .text = "Swap settings/glissando keys", .help = "[normal: (default), swap: Swap settings and glissando key location]", .options = "normal|swap"},
	{.type = 's', .name = "jack2", .text = "Pedal/MIDI jack mode (DEPRECATED: use preset)", .help = "[auto: autodetect MIDI or pedal, midi: TRS MIDI out type A, pedal_ex: expression pedal (TRS/wiper on tip), pedal_sw: sustain or 1/2/3 switch pedal, linein: forward audio to audio out]", .options = "auto|midi|pedal_ex|pedal_sw|linein", .deprecated = true},
	{.type = 'i', .name = "motion", .text = "Default motion sensor state (DEPRECATED: use preset)", .help = "0=disable, 127=internal only, else motion message interval x10ms", .lo = 0, .hi = 127, .deprecated = true},
};

// preset settings: <type>P<n><name>. The defaults are preset 1's; presets 2-8 have only their name
// and colour (kPresetValues): loading one changes only what it sets.
inline constexpr Field kPreset[] = {
	{.type = 's', .name = "name", .text = "Preset name"},
	{.type = 'h', .name = "color", .text = "LED color", .help = "hexadecimal rgb representation"},
	{.type = 'i', .name = "Mpgm", .text = "Send MIDI program change", .help = "[0-127]", .lo = 0, .hi = 127},
	{.type = 'i', .name = "Mint", .value = "1", .text = "MIDI message interval", .help = "in ms [1-127]", .lo = 1, .hi = 127},
	{.type = 'i', .name = "Mmint", .value = "127", .text = "MIDI motion sensor message interval", .help = " 0 = disable, 127 only internal, else x10ms [0-127]", .lo = 0, .hi = 127},
	{.type = 's', .name = "Mmode", .value = "mpe", .text = "MIDI mode", .help = "[mpe/normal/mono]", .options = "|mpe|normal|mono"},
	{.type = 's', .name = "Mnote", .value = "default", .text = "MIDI note mode", .help = "[default/tuning/button]", .options = "|default|tuning|button"},
	{.type = 's', .name = "jack2", .value = "auto", .text = "Pedal/MIDI jack mode", .help = "[auto: autodetect MIDI or pedal, midi: TRS MIDI out type A, pedal_ex: expression pedal (TRS/wiper on tip), pedal_sw: sustain or 1/2/3 switch pedal, linein: forward audio to audio out]", .options = "|auto|midi|pedal_ex|pedal_sw|linein"},
	{.type = 'i', .name = "tunin", .value = "0", .text = "Load tuning", .help = "[0-8]", .lo = 0, .hi = 8},
	{.type = 'f', .name = "Toff", .text = "Additional tuning offset", .help = "in cents", .lo = -4800, .hi = 4800},
	{.type = 's', .name = "flip", .value = "no", .text = "Flip layout 180 degrees", .help = "[no/yes]", .options = "|no|yes"},
	{.type = 'i', .name = "Mpres", .value = "121", .text = "MIDI CC for key pressure", .help = "Special values 127 = Disable, 120 = Polyphonic Pressure, 121 = Channel Pressure", .lo = 0, .hi = 127},
	{.type = 'i', .name = "Mx", .value = "122", .text = "MIDI CC for key X movement", .help = "Special values 127 = Disable, 122 = Pitch Bend", .lo = 0, .hi = 127},
	{.type = 'i', .name = "My", .value = "74", .text = "MIDI CC for key Y movement", .help = "Special values 127 = Disable", .lo = 0, .hi = 127},
	{.type = 'i', .name = "Mvelo", .value = "127", .text = "MIDI continuous velocity", .help = "Special values 127 = Disable, else send continuous velocity on CC73 and continuous release velocity on CC72", .lo = 0, .hi = 127},
	{.type = 'i', .name = "voice", .value = "6", .text = "Voice count/MPE channel count", .help = "[1-15]", .lo = 1, .hi = 15},
	{.type = 'i', .name = "MChan", .value = "2", .text = "MIDI channel", .help = "[1-16] (MPE: first voice channel, normally 2)", .lo = 1, .hi = 16},
	{.type = 'i', .name = "MPEpb", .value = "48", .text = "MPE pitch bend range", .help = "[12/24/48/96]", .lo = 1, .hi = 127},
	{.type = 'f', .name = "thres", .value = "0.0", .text = "Key sensitivity threshold", .help = "higher value increases note trigger force [0.0-0.5]", .lo = 0, .hi = 0.5},
	{.type = 'f', .name = "bendS", .value = "0.25", .text = "Pitch bend range", .help = "in semitones [-4.0-4.0]", .lo = -10, .hi = 10},
	{.type = 'f', .name = "presS", .value = "1.0", .text = "Key pressure factor", .help = "[0.0-4.0]", .lo = 0, .hi = 10},
	{.type = 'f', .name = "veloS", .value = "1.0", .text = "Key velocity factor", .help = "[0.0-4.0]", .lo = 0, .hi = 10},
	{.type = 'f', .name = "tiltS", .value = "1.0", .text = "Key tilt (y) factor", .help = "[-4.0-4.0]", .lo = -10, .hi = 10},
	{.type = 'i', .name = "veloO", .value = "0", .text = "MIDI velocity offset", .help = "[0-127]", .lo = 0, .hi = 127},
	{.type = 'f', .name = "volum", .value = "90.0", .text = "Volume", .help = "[0.0-127.0]", .lo = 0, .hi = 255},
	{.type = 'f', .name = "decay", .value = "32.0", .text = "Decay", .help = "[0.0-127.0]", .lo = 0, .hi = 255},
	{.type = 'f', .name = "decaP", .value = "96.0", .text = "Pedal decay", .help = "[0.0-127.0]", .lo = 0, .hi = 255},
};

// presets 1-8: the defaults of the first fields of kPreset (name, color)
inline constexpr int kPresets = 8;
inline constexpr const char* kPresetValues[kPresets][2] = {
	{"preset1", "#380000"},
	{"preset2", "#342000"},
	{"preset3", "#383800"},
	{"preset4", "#0e6000"},
	{"preset5", "#003838"},
	{"preset6", "#000ea8"},
	{"preset7", "#300064"},
	{"preset8", "#2a2a2a"},
};

// tuning settings: <type>T<n><name>, and an offset in cents per key: fT<n><key name>
inline constexpr Field kTuning[] = {
	{.type = 's', .name = "name"},
	{.type = 'h', .name = "color", .text = "LED color", .help = "hexadecimal rgb representation"},
	{.type = 'f', .name = "off", .text = "Tuning offset", .help = "offset in cents"},
	{.type = 'f', .name = "oct", .text = "Octave interval", .help = "octave interval in cents"},
	{.type = 'f', .name = "fifth", .text = "Fifth interval", .help = "fifth interval in cents"},
	{.type = 'f', .name = "", .value = "0.0", .per_key = true},
};

// tunings 1-8: the defaults of the first fields of kTuning (name, color, off, oct, fifth)
inline constexpr int kTunings = 8;
inline constexpr const char* kTuningValues[kTunings][5] = {
	{"31tet", "#005555", "0.0", "1200.0", "696.7742"},     // 1200/31*18, very close to quarter comma meantone (696.5784)
	{"19tet", "#0000aa", "0.0", "1200.0", "694.7368"},     // 1200/19*11
	{"pythagor", "#555500", "0.0", "1200.0", "701.9550"},  // pythagorean (pure fifth): 1200*log2(3/2)
	{"5tet", "#aa0000", "0.0", "1200.0", "720.0"},         // 1200/5*3
	{"7tet", "#550055", "0.0", "1200.0", "685.7142"},      // 1200/7*4
	{"BP 13tet", "#55aa00", "0.0", "1024.130", "585.2169"}, // Bohlen-Pierce by thirds: 1200*log2(3)/13*7, 1200*log2(3)/13*4
	{"BP 13tet", "#55aa00", "0.0", "1901.955", "1024.130"}, // Bohlen-Pierce by sixths: 1200*log2(3), 1200*log2(3)/13*7
	{"7limitJI", "#00aa55", "0.0", "1200.0", "696.7741"},   // 7-limit just intonation (the key offsets: kTuningNotes)
};

// the tunings' key offsets by note name (kNoteNames: C_ Db C# ... B_, the same in every octave);
// a tuning without them has the per-key field's default (0.0)
inline constexpr const char* kTuningNotes[kTunings][17] = {
	{},
	{},
	{},
	{},
	{},
	{},
	{},
	{"0.0", "-4.39774", "-6.74692", "+10.3616", "+5.96386", "-4.09683", "-0.78306", "-5.18080", "-1.86703", "+1.86703", "+5.18080", "+0.78306", "-1.56612", "-5.96386", "-10.3616", "+1.08397", "+4.39774"},
};

// calibration: fC<name><key name>, per key on top of the factory calibration
inline constexpr Field kCalibration[] = {
	{.type = 'f', .name = "fac", .value = "1.0", .per_key = true},  // force factor
	{.type = 'f', .name = "off", .value = "0.0", .per_key = true},  // zero offset (threshold)
};

} // namespace config_schema
// clang-format on
