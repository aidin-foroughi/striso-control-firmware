/*
 * The USB console's grammar: "Stc", a command byte, then the command's payload if it has one.
 * The protocol is the one Striso Live (protocol.dart) and tools/striso.py speak; the reply lines are
 * the handlers' (console.cpp) and are byte for byte what pconnection.c printed.
 *
 * HAL free, so the grammar is tested and fuzzed on the PC (tests/test_console.cpp): the board's
 * handlers and output stream are console.cpp's, the tests bring their own.
 *
 * - kCommands<H> is the one table of commands: code, payload length, handler, flags. H gives the
 *   handlers (static functions) and their output type H::Out.
 * - Console<H>::feed() takes one byte at a time. A prefix or a payload that stops arriving for
 *   longer than the stale interval is dropped (a short command, a killed tool, a USB
 *   re-enumeration in between: R6), so it can't swallow the next command's bytes. An 'S' inside a
 *   partial prefix starts the prefix again ("SStcV": R5). Payload bytes are never read as
 *   commands, and nothing but a whole "Stc" prefix starts a command.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace console {

enum Flags : uint8_t {
	kNone = 0,
	// Destroys state or resets the board: C (every key's calibration), B (into the UF2 bootloader),
	// D. R7 proposes a confirmation payload for them; that changes the protocol, so it is not done.
	kDangerous = 1u << 0,
	// The binary stream is turned off and its unsent output dropped before the reply (V: a host
	// that connects asks for the version first, and must not get a stream left on before it).
	kStopsStream = 1u << 1,
	// The command is every byte 0x80..0xFF, its code included in the request (voice 0x80 + n).
	kHighBytes = 1u << 2,
};

inline constexpr size_t kMaxPayload = 6;

struct Request {
	uint8_t code;
	std::span<const uint8_t> payload;
};

template <class Out>
struct Command {
	uint8_t code;
	uint8_t payload_len;
	void (*run)(Out&, const Request&);
	uint8_t flags;
};

// clang-format off
template <class H>
inline constexpr Command<typename H::Out> kCommands[] = {
	// code  payload  handler                  flags          reply
	{'D',    0, &H::dfu,                    kDangerous},   // "Resetting to DFU mode..." (no DFU)
	{'B',    0, &H::bootloader,             kDangerous},   // a line, then the UF2 bootloader
	{'S',    0, &H::stream_on,              kNone},        // none: the binary stream on
	{'s',    0, &H::stream_off,             kNone},        // "Stcs": the stream and the monitor off
	{'M',    0, &H::monitor_on,             kNone},        // none: the stream and the MIDI monitor on
	{'m',    0, &H::monitor_off,            kNone},        // none
	{'V',    0, &H::version,                kStopsStream}, // version, UID, id, calibration date
	{'I',    0, &H::threads,                kNone},        // the thread list
	{'N',    0, &H::tuning,                 kNone},        // the tuning and every key's pitch
	{'K',    1, &H::settings_key,           kNone},        // key id: "K ok|err <id>"
	{'X',    0, &H::settings_layout,        kNone},        // the settings-key layout
	{'Y',    0, &H::state,                  kNone},        // the board's state in one line
	{'y',    0, &H::state_counter,          kNone},        // "y <n>"
	{'P',    0, &H::profile,                kNone},        // the synth's profile since the last P
	{'p',    0, &H::voice_params,           kNone},        // the current voice's parameters
	{'f',    0, &H::fx_params,              kNone},        // the effects' parameters
	{'g',    0, &H::touch_params,           kNone},        // the touch parameters (target 2)
	{'k',    0, &H::key_params,             kNone},        // the key factors (target 3)
	{'Q',    6, &H::set_param,              kNone},        // target, index, float32 LE: "Q err" or none
	{'v',    0, &H::voices,                 kNone},        // the voices, '*' the current one
	{0x80,   0, &H::select_voice,           kHighBytes},   // "voice <n> ..." or "no voice <n>"
	{'L',    0, &H::load,                   kNone},        // the synth's load since the last L
	{'U',    0, &H::audio_status,           kNone},        // USB audio statistics
	{'t',    3, &H::audio_target,           kNone},        // 3 digits: "usb audio target ..."
	{'l',    1, &H::audio_latency,          kNone},        // '1' a marker, else an echo
	{'u',    0, &H::audio_tone,             kNone},        // the test tone on/off
	{'C',    0, &H::calibration_reset,      kDangerous},   // every key's calibration to the default
	{'E',    0, &H::scanner_stats,          kNone},        // the key scanner's counters
	{'Z',    0, &H::scanner_keys,           kNone},        // every button's calibration state
	{'R',    2, &H::scanner_record,         kNone},        // 2 digits: record seconds, 00 dump
};
// clang-format on

template <class Out>
constexpr const Command<Out>* find(std::span<const Command<Out>> table, uint8_t c) {
	for (const Command<Out>& e : table) {
		if (c >= 0x80 ? (e.flags & kHighBytes) != 0 : e.code == c) return &e;
	}
	return nullptr;
}

// unique codes below 0x80, at most one kHighBytes entry (code 0x80), payloads that fit
template <class Out>
constexpr bool valid(std::span<const Command<Out>> table) {
	int high = 0;
	for (size_t i = 0; i < table.size(); i++) {
		const Command<Out>& e = table[i];
		if (e.run == nullptr || e.payload_len > kMaxPayload) return false;
		if ((e.flags & kHighBytes) != 0) {
			if (e.code != 0x80) return false;
			high++;
		} else if (e.code >= 0x80) {
			return false;
		}
		for (size_t j = 0; j < i; j++) {
			if (table[j].code == e.code) return false;
		}
	}
	return high <= 1;
}

template <class H>
class Console {
public:
	using Out = typename H::Out;
	using Cmd = Command<Out>;
	static constexpr std::span<const Cmd> table{kCommands<H>};
	static_assert(valid(table), "console::kCommands: duplicate code, bad payload length or flags");

	// out: where the replies go; stale: a prefix or payload is dropped after this many ticks
	// without a byte
	constexpr Console(Out& out, uint32_t stale) :
	    out_(out), stale_(stale) {}

	// One byte from the host, at time now (system ticks, free running: wraps). Returns the
	// command it completed and ran, or nullptr.
	const Cmd* feed(uint8_t c, uint32_t now) {
		if (state_ != State::idle && now - last_ > stale_) state_ = State::idle;
		last_ = now;
		switch (state_) {
		case State::idle:
			if (c == 'S') state_ = State::s;
			return nullptr;
		case State::s:
			state_ = c == 't' ? State::st : prefix_restart(c);
			return nullptr;
		case State::st:
			state_ = c == 'c' ? State::stc : prefix_restart(c);
			return nullptr;
		case State::stc: {
			state_ = State::idle;
			const Cmd* cmd = find(table, c);
			if (cmd == nullptr) return nullptr;
			code_ = c;
			if (cmd->payload_len == 0) return run(*cmd);
			pending_ = cmd;
			len_ = 0;
			state_ = State::payload;
			return nullptr;
		}
		case State::payload:
			payload_[len_++] = c;
			if (len_ < pending_->payload_len) return nullptr;
			state_ = State::idle;
			return run(*pending_);
		}
		return nullptr;
	}

	// no partial prefix or payload pending
	[[nodiscard]] bool idle() const { return state_ == State::idle; }

private:
	enum class State : uint8_t { idle,
		s,
		st,
		stc,
		payload };

	static State prefix_restart(uint8_t c) { return c == 'S' ? State::s : State::idle; }

	// not inlined: the handlers are called through the table here and only here (the build's
	// stack check is told where these indirect calls go, cmake/firmware_checks.cmake)
	__attribute__((noinline)) const Cmd* run(const Cmd& cmd) {
		out_.begin();
		if ((cmd.flags & kStopsStream) != 0) H::stop_stream();
		cmd.run(out_, Request{code_, std::span<const uint8_t>(payload_, cmd.payload_len)});
		return &cmd;
	}

	Out& out_;
	uint32_t stale_;
	uint32_t last_ = 0;
	State state_ = State::idle;
	uint8_t code_ = 0;
	uint8_t len_ = 0;
	const Cmd* pending_ = nullptr;
	uint8_t payload_[kMaxPayload] = {};
};

} // namespace console
