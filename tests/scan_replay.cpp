// scan_replay <recording.ksr> [<stream.csv>] [--quiet]
//
// Replays a raw scan recording from the board (StcR, tools/record_raw.py) through the KeyScanner
// and checks that it gives exactly what the board did:
//   - per scan, the number of messages the scanner tried to send and their hash (the board
//     records both), and
//   - with the StcS stream recorded in the same session (stream.csv), the messages themselves:
//     the ones the queue took must be a contiguous run of the stream's key messages.
// Prints a summary: scans, cycles per scan, key activity, messages. Exit code 1 on any difference.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "scan_replay.hpp"

int main(int argc, char** argv) {
	if (argc < 2) {
		std::fprintf(stderr, "usage: scan_replay <recording.ksr> [<stream.csv>] [--quiet]\n");
		return 2;
	}
	const char* stream_path = nullptr;
	bool quiet = false;
	for (int i = 2; i < argc; i++) {
		if (std::strcmp(argv[i], "--quiet") == 0) quiet = true;
		else stream_path = argv[i];
	}
	std::vector<uint8_t> blob;
	{
		FILE* f = std::fopen(argv[1], "rb");
		if (!f) {
			std::fprintf(stderr, "can't read %s\n", argv[1]);
			return 2;
		}
		uint8_t buf[65536];
		size_t n = 0;
		while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) blob.insert(blob.end(), buf, buf + n);
		std::fclose(f);
	}
	const scanreplay::Result r = scanreplay::replay(blob);
	if (!r.error.empty()) {
		std::printf("FAIL %s: %s\n", argv[1], r.error.c_str());
		return 1;
	}
	uint32_t queued = 0;
	std::vector<int> per_key(ks::kButtons, 0);
	for (const scanreplay::Msg& m : r.msgs) {
		queued += m.queued;
		per_key[m.v[1]]++;
	}
	int keys = 0;
	for (int n : per_key) keys += n > 0;
	if (!quiet) {
		std::printf("%s: %u scans (%u asked s, %s), first scan %u\n", argv[1], r.frames, r.head.seconds,
		    r.head.flags & 1 ? "buffer full" : "time up", r.head.first_seq);
		std::printf("  messages %zu (%u queued, %u refused), %d keys; scans marked torn %u\n", r.msgs.size(), queued, r.refused, keys, r.torn);
	}
	if (!quiet && r.cycles.size() > 1) {
		std::vector<uint32_t> c(r.cycles.begin() + 1, r.cycles.end());
		std::sort(c.begin(), c.end());
		std::printf("  DWT cycles per scan: min %u median %u max %u (%.1f Hz at 400 MHz)\n", c.front(), c[c.size() / 2], c.back(),
		    400e6 / c[c.size() / 2]);
	}
	if (!quiet && r.has_stats) {
		// the noise floor: the keys with the highest detection sample (threshold kKeyDetect = 64)
		double worst = 0;
		int worst_b = -1;
		double mean_all = 0;
		int n_all = 0;
		for (int b = 0; b < ks::kButtons; b++) {
			const ks::Acc& a = r.stats.on[b];
			if (!a.n) continue;
			const double m = static_cast<double>(a.sum) / a.n;
			mean_all += m;
			n_all++;
			if (a.max > worst) {
				worst = a.max;
				worst_b = b;
			}
		}
		std::printf("  noise: detection sample mean %.2f over %d buttons, highest max %.0f (button %d; threshold %d)\n",
		    n_all ? mean_all / n_all : 0.0, n_all, worst, worst_b, ks::kKeyDetect);
		for (int b = 0; b < ks::kButtons; b++) {
			const ks::Acc& a = r.stats.on[b];
			const ks::Acc& p = r.stats.p[b];
			if (!a.n) continue;
			const double m = static_cast<double>(a.sum) / a.n;
			const double sd = std::sqrt(std::max(0.0, static_cast<double>(a.sumsq) / a.n - m * m));
			const double pm = p.n ? static_cast<double>(p.sum) / p.n : 0.0;
			if (!quiet && std::getenv("SCAN_NOISE_ALL")) {
				std::printf("    button %2d on mean %6.2f sd %5.2f max %4u | p n %6u mean %7.2f max %5u\n", b, m, sd, a.max, p.n, pm, p.max);
			}
		}
	}
	int bad = 0;
	if (r.mismatched) {
		std::printf("FAIL %u scans differ from the board's message count/hash (first: scan %u)\n", r.mismatched, r.first_mismatch);
		bad++;
	} else if (!quiet) {
		std::printf("  every scan: the same message count and hash as on the board\n");
	}
	if (stream_path) {
		const std::vector<std::array<int, 6>> stream = scanreplay::read_stream_csv(stream_path);
		int matches = 0;
		const long off = scanreplay::find_in_stream(r.msgs, stream, &matches);
		if (off < 0) {
			std::printf("FAIL the replayed messages are not a run of the stream's %zu key messages\n", stream.size());
			bad++;
		} else if (!quiet) {
			std::printf("  stream: the %u queued messages are the stream's key messages %ld..%ld of %zu (%d place%s)\n", queued, off,
			    off + static_cast<long>(queued), stream.size(), matches, matches == 1 ? "" : "s");
		}
	}
	if (!bad && !quiet) std::printf("RAW: exact\n");
	return bad ? 1 : 0;
}
