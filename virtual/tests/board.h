// The virtual board in the tests: a board, console commands with their replies, renders.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "striso_virtual.h"

namespace vt {

struct Board {
	sv_board* b = nullptr;
	explicit Board(const sv_options* o = nullptr) : b(sv_create(o)) {}
	~Board() { sv_destroy(b); }
	Board(const Board&) = delete;
	Board& operator=(const Board&) = delete;

	// everything the board sent to the host so far
	std::string read() {
		std::string out;
		uint8_t buf[4096];
		int32_t n;
		while ((n = sv_console_read(b, buf, sizeof buf)) > 0) out.append(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
		return out;
	}
	// sends bytes, runs the board for ms of virtual time, returns what came back
	std::string send(const std::string& bytes, int ms = 20) {
		read();
		sv_console_write(b, reinterpret_cast<const uint8_t*>(bytes.data()), static_cast<int32_t>(bytes.size()));
		sv_advance_ms(b, ms);
		return read();
	}
	std::string cmd(const std::string& c, int ms = 20) { return send("Stc" + c, ms); }
	void run_ms(int ms) { sv_advance_ms(b, ms); }
};

inline std::vector<std::string> lines(const std::string& s) {
	std::vector<std::string> out;
	size_t p = 0;
	while (p < s.size()) {
		size_t e = s.find("\r\n", p);
		if (e == std::string::npos) {
			out.push_back(s.substr(p));
			break;
		}
		out.push_back(s.substr(p, e - p));
		p = e + 2;
	}
	return out;
}

} // namespace vt
