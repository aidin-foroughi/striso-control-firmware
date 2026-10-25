// Standard MIDI Files (format 0 and 1): the channel messages of every track, merged into one list
// in time order, with the tempo map applied (seconds). Meta events other than tempo and SysEx are
// skipped.
#ifndef STRISO_SDK_SMF_H_
#define STRISO_SDK_SMF_H_

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace striso::sdk {

struct MidiEvent {
	double t; // seconds from the start
	uint8_t bytes[3];
	uint8_t len;
};

struct MidiFile {
	int format = 0, tracks = 0, division = 0;
	std::vector<MidiEvent> events;
	double seconds = 0; // the last event's time (end of track meta events included)
	std::string error;  // empty: read
};

[[nodiscard]] MidiFile parse_midi(std::span<const uint8_t> data);
[[nodiscard]] MidiFile read_midi_file(const char* path);

// a format 0 file of the events (for tests and tools): 480 ticks per quarter at 120 bpm
[[nodiscard]] std::vector<uint8_t> make_midi(std::span<const MidiEvent> events);

} // namespace striso::sdk

#endif // STRISO_SDK_SMF_H_
