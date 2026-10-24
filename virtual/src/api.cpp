// striso_virtual's front: boards, their firmware instances, the bootloader's drive, real time.
//
// A board runs one firmware instance at a time: a private copy of this very library, loaded from
// a temporary file, whose svfw_api() boots the firmware with the copy's own (fresh) globals. A
// reset ends the instance and unloads the copy; after the bootloader's pause a new copy boots. The
// config flash, the device id and the RTC backup registers are the board's (sv_shared) and
// survive. The library this front runs in never runs firmware itself.
//
// After StcB (the RTC signature set before the reset) the board stays in the bootloader: a
// directory stands in for the UF2 drive (INFO_UF2.TXT, CONFIG.UF2); a complete UF2 file written
// there is flashed (blocks in the config area: the area is erased and written, as the bootloader
// does with its flash sector; other blocks are firmware and are not run) and the board restarts.
#include "striso_virtual.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "fw_api.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

thread_local std::string last_error;

constexpr uint32_t UF2_MAGIC0 = 0x0A324655, UF2_MAGIC1 = 0x9E5D5157, UF2_MAGIC_END = 0x0AB16F30;
constexpr uint32_t UF2_FAMILY = 0xA21E1295, UF2_FLAG_FAMILY = 0x2000;
constexpr uint64_t BOOTLOADER_FRAMES = 4800; // 100 ms: the bootloader's checks before the app starts

// --- a private copy of this library --------------------------------------------------------------

struct Module {
#if defined(_WIN32)
	HMODULE h = nullptr;
#else
	void* h = nullptr;
#endif
	fs::path file;
	const sv_fw_api* api = nullptr;
};

fs::path self_path() {
#if defined(_WIN32)
	HMODULE h = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    reinterpret_cast<LPCWSTR>(&sv_api_version), &h);
	std::wstring p(32768, L'\0');
	const DWORD n = GetModuleFileNameW(h, p.data(), static_cast<DWORD>(p.size()));
	p.resize(n);
	return fs::path(p);
#else
	Dl_info info{};
	if (!dladdr(reinterpret_cast<void*>(&sv_api_version), &info) || !info.dli_fname) return {};
	return fs::path(info.dli_fname);
#endif
}

uint64_t pid() {
#if defined(_WIN32)
	return GetCurrentProcessId();
#else
	return static_cast<uint64_t>(getpid());
#endif
}

fs::path copies_dir() {
	std::error_code ec;
	fs::path d = fs::temp_directory_path(ec) / "striso_virtual";
	fs::create_directories(d, ec);
	return d;
}

// copies of exited processes (Windows keeps a loaded DLL's file: those still in use stay)
void clean_stale_copies() {
	static std::once_flag once;
	std::call_once(once, [] {
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(copies_dir(), ec)) {
			const std::string n = e.path().filename().string();
			if (n.rfind("fw_", 0) != 0) continue;
			const std::string mine = "fw_" + std::to_string(pid()) + "_";
			if (n.rfind(mine, 0) == 0) continue;
			// fw_<pid>_<n>: skip copies of processes that still run
			const uint64_t other = std::strtoull(n.c_str() + 3, nullptr, 10);
#if defined(_WIN32)
			if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(other))) {
				DWORD code = 0;
				const bool alive = GetExitCodeProcess(p, &code) && code == STILL_ACTIVE;
				CloseHandle(p);
				if (alive) continue;
			}
#else
			if (other && kill(static_cast<pid_t>(other), 0) == 0) continue;
#endif
			fs::remove(e.path(), ec);
		}
	});
}

bool load(Module& m) {
	static std::atomic<uint32_t> counter{0};
	clean_stale_copies();
	const fs::path src = self_path();
	if (src.empty()) {
		last_error = "striso_virtual: cannot find its own file";
		return false;
	}
	m.file = copies_dir() / ("fw_" + std::to_string(pid()) + "_" + std::to_string(++counter) + src.extension().string());
	std::error_code ec;
	fs::copy_file(src, m.file, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		last_error = "striso_virtual: cannot copy " + src.string() + " to " + m.file.string() + ": " + ec.message();
		return false;
	}
#if defined(_WIN32)
	m.h = LoadLibraryW(m.file.wstring().c_str());
	if (!m.h) {
		last_error = "striso_virtual: LoadLibrary " + m.file.string() + " failed (" + std::to_string(GetLastError()) + ")";
		fs::remove(m.file, ec);
		return false;
	}
	auto f = reinterpret_cast<const sv_fw_api* (*)(void)>(reinterpret_cast<void*>(GetProcAddress(m.h, "svfw_api")));
#else
	m.h = dlopen(m.file.c_str(), RTLD_NOW | RTLD_LOCAL);
	if (!m.h) {
		const char* e = dlerror();
		last_error = std::string("striso_virtual: dlopen failed: ") + (e ? e : "?");
		fs::remove(m.file, ec);
		return false;
	}
	auto f = reinterpret_cast<const sv_fw_api* (*)(void)>(dlsym(m.h, "svfw_api"));
#endif
	m.api = f ? f() : nullptr;
	if (!m.api || m.api->version != SV_FW_API_VERSION) {
		last_error = "striso_virtual: the copy has no matching svfw_api";
		return false;
	}
	return true;
}

void unload(Module& m) {
#if defined(_WIN32)
	if (m.h) FreeLibrary(m.h);
#else
	if (m.h) dlclose(m.h);
#endif
	m.h = nullptr;
	m.api = nullptr;
	std::error_code ec;
	if (!m.file.empty()) fs::remove(m.file, ec);
	m.file.clear();
}

// --- UF2 --------------------------------------------------------------------------------------------

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
void wr32(uint8_t* p, uint32_t v) {
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = v >> 24;
}

// the config area as the bootloader's CONFIG.UF2 shows it: 256-byte blocks up to the end marker
std::vector<uint8_t> config_uf2(const uint8_t* flash) {
	size_t end = 256;
	for (size_t o = 0; o + 16 <= SV_FLASH_SIZE; o += 16) {
		if (std::memcmp(flash + o, "MCfgEnd ", 8) == 0) {
			end = o + 16;
			break;
		}
		if (flash[o] < 32 || flash[o] >= 127) {
			end = o;
			break;
		}
	}
	const uint32_t nblocks = static_cast<uint32_t>(std::max<size_t>((end + 255) / 256, 1));
	std::vector<uint8_t> out(static_cast<size_t>(nblocks) * 512, 0);
	for (uint32_t k = 0; k < nblocks; k++) {
		uint8_t* b = &out[static_cast<size_t>(k) * 512];
		wr32(b, UF2_MAGIC0);
		wr32(b + 4, UF2_MAGIC1);
		wr32(b + 8, UF2_FLAG_FAMILY);
		wr32(b + 12, SV_FLASH_ADDR + 256 * k);
		wr32(b + 16, 256);
		wr32(b + 20, k);
		wr32(b + 24, nblocks);
		wr32(b + 28, UF2_FAMILY);
		std::memcpy(b + 32, flash + 256 * k, 256);
		wr32(b + 508, UF2_MAGIC_END);
	}
	return out;
}

// --- boards -------------------------------------------------------------------------------------------

enum class Phase { running, rebooting, bootloader };

} // namespace

struct sv_board {
	std::mutex m;
	sv_shared* shared = nullptr;
	Module mod;
	Phase phase = Phase::rebooting;
	uint64_t time = 0, phase_until = 0, last_poll = 0;
	int32_t resets = 0;
	uint32_t generation = 0;
	bool attached = false;
	fs::path drive;
	bool drive_own = false;
	// real time
	std::thread rt;
	std::atomic<bool> rt_run{false};
	std::vector<float> fifo;
	uint64_t fifo_w = 0, fifo_r = 0;
	size_t fifo_frames = 0;
	// sv_audio_source at another rate: rendered frames not yet used, the read position in them
	std::vector<float> rs;
	double rs_pos = 0;
};

namespace {

void note_usb(sv_board* b) {
	const bool now = b->phase == Phase::running && b->mod.api && b->mod.api->usb_attached();
	if (now && !b->attached) b->generation++;
	b->attached = now;
}

bool boot(sv_board* b) {
	if (!load(b->mod)) return false;
	b->phase = Phase::running;
	b->mod.api->boot(b->shared);
	note_usb(b);
	return true;
}

void write_drive(sv_board* b) {
	std::error_code ec;
	fs::create_directories(b->drive, ec);
	{
		std::ofstream f(b->drive / "INFO_UF2.TXT", std::ios::binary);
		f << "UF2 Bootloader (virtual)\r\nModel: Striso Control (virtual board)\r\nBoard-ID: STM32H743xI-Striso-virtual\r\n";
	}
	{
		const std::vector<uint8_t> c = config_uf2(b->shared->flash);
		std::ofstream f(b->drive / "CONFIG.UF2", std::ios::binary);
		f.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
	}
	{
		std::ofstream f(b->drive / "INDEX.HTM", std::ios::binary);
		f << "<html><body>Striso virtual board: the bootloader's drive</body></html>\r\n";
	}
}

void clear_drive(sv_board* b) {
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(b->drive, ec)) fs::remove_all(e.path(), ec);
	if (b->drive_own) fs::remove(b->drive, ec);
}

void end_instance(sv_board* b) {
	if (b->mod.api) b->mod.api->shutdown();
	unload(b->mod);
	b->resets++;
	note_usb(b);
	if (b->shared->rtc_bkp[0] == SV_BOOTLOADER_SIGNATURE) {
		b->shared->rtc_bkp[0] = 0; // the bootloader takes the signature
		b->phase = Phase::bootloader;
		write_drive(b);
	} else {
		b->phase = Phase::rebooting;
		b->phase_until = b->time + BOOTLOADER_FRAMES;
	}
}

// a complete UF2 on the drive: flash it and restart. true when it did.
bool poll_drive(sv_board* b) {
	std::error_code ec;
	for (const auto& e : fs::directory_iterator(b->drive, ec)) {
		const std::string name = e.path().filename().string();
		std::string upper = name;
		for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		if (upper == "CONFIG.UF2" || upper == "CURRENT.UF2" || upper.size() < 4 || upper.substr(upper.size() - 4) != ".UF2") continue;
		std::ifstream f(e.path(), std::ios::binary);
		std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		f.close();
		if (d.empty() || d.size() % 512) continue;
		const size_t n = d.size() / 512;
		std::vector<bool> seen;
		uint32_t total = 0;
		bool ok = true, config = false;
		for (size_t k = 0; k < n && ok; k++) {
			const uint8_t* blk = &d[k * 512];
			if (rd32(blk) != UF2_MAGIC0 || rd32(blk + 4) != UF2_MAGIC1 || rd32(blk + 508) != UF2_MAGIC_END) ok = false;
			const uint32_t no = rd32(blk + 20), num = rd32(blk + 24), addr = rd32(blk + 12), size = rd32(blk + 16);
			if (!ok || size > 476 || num == 0 || no >= num) {
				ok = false;
				break;
			}
			if (total == 0) {
				total = num;
				seen.assign(num, false);
			}
			if (num != total) ok = false;
			else seen[no] = true;
			if (addr >= SV_FLASH_ADDR && addr < SV_FLASH_ADDR + SV_FLASH_SIZE) config = true;
		}
		if (!ok || std::find(seen.begin(), seen.end(), false) != seen.end()) continue; // not (yet) complete
		if (config) std::memset(b->shared->flash, 0xff, SV_FLASH_SIZE); // the sector is erased first
		for (size_t k = 0; k < n; k++) {
			const uint8_t* blk = &d[k * 512];
			const uint32_t addr = rd32(blk + 12), size = rd32(blk + 16);
			if (addr >= SV_FLASH_ADDR && addr + size <= SV_FLASH_ADDR + SV_FLASH_SIZE)
				std::memcpy(b->shared->flash + (addr - SV_FLASH_ADDR), blk + 32, size);
		}
		clear_drive(b);
		b->phase = Phase::rebooting;
		b->phase_until = b->time + BOOTLOADER_FRAMES;
		return true;
	}
	return false;
}

int32_t advance_locked(sv_board* b, int32_t frames, float* usb, float* engine, float* fx) {
	int32_t done = 0;
	while (done < frames) {
		const int32_t left = frames - done;
		float* u = usb ? usb + 2 * done : nullptr;
		float* e = engine ? engine + done : nullptr;
		float* x = fx ? fx + 2 * done : nullptr;
		if (b->phase == Phase::running) {
			const int32_t n = b->mod.api->advance(left, u, e, x);
			done += n;
			b->time += static_cast<uint64_t>(n);
			note_usb(b);
			if (b->mod.api->reset_requested()) end_instance(b);
			continue;
		}
		int32_t n = left;
		if (b->phase == Phase::rebooting) n = static_cast<int32_t>(std::min<uint64_t>(static_cast<uint64_t>(left), b->phase_until - b->time));
		if (u) std::fill(u, u + 2 * n, 0.0f);
		if (e) std::fill(e, e + n, 0.0f);
		if (x) std::fill(x, x + 2 * n, 0.0f);
		done += n;
		b->time += static_cast<uint64_t>(n);
		if (b->phase == Phase::rebooting && b->time >= b->phase_until) {
			if (!boot(b)) return SV_ELOAD;
		} else if (b->phase == Phase::bootloader && b->time - b->last_poll >= 2400) {
			b->last_poll = b->time;
			poll_drive(b);
		}
	}
	return done;
}

void rt_loop(sv_board* b) {
	using clk = std::chrono::steady_clock;
	const auto t0 = clk::now();
	uint64_t ran = 0;
	std::vector<float> buf;
	while (b->rt_run.load()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		const uint64_t due = static_cast<uint64_t>(std::chrono::duration<double>(clk::now() - t0).count() * SV_SAMPLE_RATE);
		if (due <= ran) continue;
		uint64_t n = due - ran;
		if (n > 4800) { // more than 100 ms behind (a debugger, a sleep): skip ahead
			ran = due - 4800;
			n = 4800;
		}
		buf.resize(static_cast<size_t>(n) * 2);
		std::lock_guard<std::mutex> lk(b->m);
		const int32_t got = advance_locked(b, static_cast<int32_t>(n), buf.data(), nullptr, nullptr);
		ran += n;
		for (int32_t k = 0; k < got; k++) {
			if (b->fifo_w - b->fifo_r >= b->fifo_frames) b->fifo_r++; // full: the oldest goes
			const size_t i = static_cast<size_t>(b->fifo_w % b->fifo_frames);
			b->fifo[2 * i] = buf[2 * static_cast<size_t>(k)];
			b->fifo[2 * i + 1] = buf[2 * static_cast<size_t>(k) + 1];
			b->fifo_w++;
		}
	}
}

void put_le16(uint8_t* p, uint16_t v) {
	p[0] = v & 0xff;
	p[1] = v >> 8;
}

// the virtual board's device id and factory calibration (flash at 0x081e0000)
void make_devspec(sv_shared* s, uint32_t serial) {
	s->uid[0] = serial;
	s->uid[1] = 0x4C415554; // "TUAL"
	s->uid[2] = 0x56495254; // "VIRT"
	std::memset(s->devspec, 0, sizeof s->devspec);
	uint8_t* id = s->devspec; // devspec_id_t
	for (int i = 0; i < 3; i++) wr32(id + 4 * i, s->uid[i]);
	std::memcpy(id + 12, "virtual         ", 16);
	std::memcpy(id + 28, "Striso board", 12);
	put_le16(id + 92, 1);                    // hardware_revision 1: 1k ADC pull-ups
	put_le16(id + 254, (1 << 18) / 64 + 1);  // base_calib_force (CALIB_FORCE)
	uint8_t* cal = s->devspec + 256; // calib_dis_force: per key, type 0x02 (>= v2.1)
	for (int i = 0; i < 3; i++) wr32(cal + 4 * i, s->uid[i]);
	for (int k = 0; k < 68; k++) put_le16(cal + 12 + 2 * k, 2048);
	put_le16(cal + 256 - 16 - 2, 0x02);
	std::memcpy(cal + 256 - 16, "virtual", 7);
	std::memset(s->devspec + 512, 0xff, 512); // no breakpoint calibration
}

} // namespace

extern "C" {

SV_API int32_t sv_api_version(void) { return SV_API_VERSION; }

SV_API const char* sv_firmware_version(void) { return svfw_api()->firmware_version(); }

SV_API const char* sv_last_error(void) { return last_error.c_str(); }

SV_API sv_board* sv_create(const sv_options* o) {
	last_error.clear();
	sv_options opt{};
	opt.size = sizeof opt;
	opt.boot_ms = 1000;
	if (o) std::memcpy(&opt, o, std::min<size_t>(o->size ? o->size : sizeof opt, sizeof opt));
	auto* b = new sv_board;
	b->shared = new sv_shared{};
	std::memset(b->shared->flash, 0xff, SV_FLASH_SIZE);
	if (opt.flash && opt.flash_len) std::memcpy(b->shared->flash, opt.flash, std::min<uint32_t>(opt.flash_len, SV_FLASH_SIZE));
	make_devspec(b->shared, opt.serial ? opt.serial : 1);
	b->shared->port_key_held = opt.port_key_held;
	static std::atomic<uint32_t> boards{0};
	if (opt.drive_dir && *opt.drive_dir) {
		b->drive = fs::path(opt.drive_dir);
	} else {
		b->drive = copies_dir() / ("drive_" + std::to_string(pid()) + "_" + std::to_string(++boards));
		b->drive_own = true;
	}
	if (!boot(b)) {
		delete b->shared;
		delete b;
		return nullptr;
	}
	if (opt.boot_ms == 0) opt.boot_ms = 1000;
	if (opt.boot_ms > 0) advance_locked(b, opt.boot_ms * (SV_SAMPLE_RATE / 1000), nullptr, nullptr, nullptr);
	return b;
}

SV_API void sv_destroy(sv_board* b) {
	if (!b) return;
	sv_realtime_stop(b);
	{
		std::lock_guard<std::mutex> lk(b->m);
		if (b->mod.api) b->mod.api->shutdown();
		unload(b->mod);
		if (b->phase == Phase::bootloader) clear_drive(b);
	}
	delete b->shared;
	delete b;
}

SV_API int32_t sv_render_taps(sv_board* b, int32_t frames, float* usb, float* engine, float* fx) {
	if (!b || frames < 0) return SV_EINVAL;
	if (b->rt_run.load()) return SV_EBUSY;
	std::lock_guard<std::mutex> lk(b->m);
	return advance_locked(b, frames, usb, engine, fx);
}

SV_API int32_t sv_render(sv_board* b, float* stereo, int32_t frames) { return sv_render_taps(b, frames, stereo, nullptr, nullptr); }

SV_API int32_t sv_advance_ms(sv_board* b, int32_t ms) {
	if (!b || ms < 0) return SV_EINVAL;
	return sv_render_taps(b, ms * (SV_SAMPLE_RATE / 1000), nullptr, nullptr, nullptr);
}

SV_API uint64_t sv_time_frames(sv_board* b) {
	if (!b) return 0;
	std::lock_guard<std::mutex> lk(b->m);
	return b->time;
}

SV_API int32_t sv_audio_source(void* board, float* stereo, int32_t frames, int32_t rate) {
	auto* b = static_cast<sv_board*>(board);
	if (!b || !stereo || frames <= 0) return 0;
	if (b->rt_run.load()) return 0;
	std::lock_guard<std::mutex> lk(b->m);
	if (rate <= 0 || rate == SV_SAMPLE_RATE) {
		const int32_t n = advance_locked(b, frames, stereo, nullptr, nullptr);
		return n < 0 ? 0 : n;
	}
	// linear interpolation: output frame k is at rs_pos + k * step in the rendered frames
	const double step = static_cast<double>(SV_SAMPLE_RATE) / rate;
	const double last = b->rs_pos + (frames - 1) * step;
	const size_t need = static_cast<size_t>(last) + 2, have = b->rs.size() / 2;
	if (need > have) {
		b->rs.resize(need * 2);
		if (advance_locked(b, static_cast<int32_t>(need - have), b->rs.data() + have * 2, nullptr, nullptr) < 0) return 0;
	}
	for (int32_t k = 0; k < frames; k++) {
		const double t = b->rs_pos + k * step;
		const size_t i = static_cast<size_t>(t);
		const float f = static_cast<float>(t - static_cast<double>(i));
		for (int c = 0; c < 2; c++) stereo[2 * k + c] = b->rs[2 * i + c] + f * (b->rs[2 * (i + 1) + c] - b->rs[2 * i + c]);
	}
	const double end = b->rs_pos + frames * step;
	const size_t used = static_cast<size_t>(end);
	b->rs.erase(b->rs.begin(), b->rs.begin() + static_cast<std::ptrdiff_t>(std::min(used, b->rs.size() / 2) * 2));
	b->rs_pos = end - static_cast<double>(used);
	return frames;
}

SV_API int32_t sv_realtime_start(sv_board* b, int32_t fifo_frames) {
	if (!b || fifo_frames < 0) return SV_EINVAL;
	if (b->rt_run.load()) return 0;
	{
		std::lock_guard<std::mutex> lk(b->m);
		b->fifo_frames = static_cast<size_t>(fifo_frames ? fifo_frames : SV_SAMPLE_RATE);
		b->fifo.assign(b->fifo_frames * 2, 0.0f);
		b->fifo_w = b->fifo_r = 0;
	}
	b->rt_run = true;
	b->rt = std::thread(rt_loop, b);
	return 0;
}

SV_API int32_t sv_realtime_stop(sv_board* b) {
	if (!b) return SV_EINVAL;
	if (b->rt_run.exchange(false) && b->rt.joinable()) b->rt.join();
	return 0;
}

SV_API int32_t sv_read_audio(sv_board* b, float* stereo, int32_t max_frames) {
	if (!b || !stereo || max_frames < 0) return SV_EINVAL;
	std::lock_guard<std::mutex> lk(b->m);
	int32_t n = 0;
	while (n < max_frames && b->fifo_r < b->fifo_w) {
		const size_t i = static_cast<size_t>(b->fifo_r % b->fifo_frames);
		stereo[2 * n] = b->fifo[2 * i];
		stereo[2 * n + 1] = b->fifo[2 * i + 1];
		b->fifo_r++;
		n++;
	}
	return n;
}

SV_API int32_t sv_usb_state(sv_board* b) {
	if (!b) return SV_EINVAL;
	std::lock_guard<std::mutex> lk(b->m);
	if (b->phase == Phase::bootloader) return SV_USB_BOOTLOADER;
	return b->attached ? SV_USB_ATTACHED : SV_USB_DETACHED;
}

SV_API uint32_t sv_usb_generation(sv_board* b) {
	if (!b) return 0;
	std::lock_guard<std::mutex> lk(b->m);
	return b->generation;
}

#define SV_WITH_FW(b, call) \
	do { \
		if (!(b)) return SV_EINVAL; \
		std::lock_guard<std::mutex> lk((b)->m); \
		if ((b)->phase != Phase::running || !(b)->mod.api) return SV_ENODEV; \
		return call; \
	} while (0)

SV_API int32_t sv_console_write(sv_board* b, const uint8_t* bytes, int32_t n) {
	if (!bytes || n < 0) return SV_EINVAL;
	SV_WITH_FW(b, b->attached ? b->mod.api->console_write(bytes, n) : SV_ENODEV);
}

SV_API int32_t sv_console_read(sv_board* b, uint8_t* out, int32_t max) {
	if (!out || max < 0) return SV_EINVAL;
	SV_WITH_FW(b, b->attached ? b->mod.api->console_read(out, max) : SV_ENODEV);
}

SV_API int32_t sv_midi_write(sv_board* b, const uint8_t* packets, int32_t n) {
	if (!packets || n < 0) return SV_EINVAL;
	SV_WITH_FW(b, b->attached ? b->mod.api->midi_write(packets, n) : SV_ENODEV);
}

SV_API int32_t sv_midi_read(sv_board* b, uint8_t* out, int32_t max) {
	if (!out || max < 0) return SV_EINVAL;
	SV_WITH_FW(b, b->attached ? b->mod.api->midi_read(out, max) : SV_ENODEV);
}

SV_API int32_t sv_inject_key(sv_board* b, int32_t src, int32_t id, int32_t pres, int32_t velo, int32_t x, int32_t y) {
	SV_WITH_FW(b, b->mod.api->inject_key(src, id, pres, velo, x, y));
}

SV_API int32_t sv_inject_control(sv_board* b, int32_t id, int32_t value) { SV_WITH_FW(b, b->mod.api->inject_control(id, value)); }

SV_API int32_t sv_inject_motion(sv_board* b, const int16_t raw[6]) {
	if (!raw) return SV_EINVAL;
	SV_WITH_FW(b, b->mod.api->inject_motion(raw));
}

SV_API int32_t sv_set_engine_controls(sv_board* b, int32_t v, const float* values) {
	SV_WITH_FW(b, b->mod.api->set_engine_controls(v, values));
}

SV_API int32_t sv_select_voice(sv_board* b, int32_t n) { SV_WITH_FW(b, b->mod.api->select_voice(n)); }

SV_API int32_t sv_get_leds(sv_board* b, sv_leds* out) {
	if (!out) return SV_EINVAL;
	SV_WITH_FW(b, b->mod.api->get_leds(out));
}

SV_API int32_t sv_get_state(sv_board* b, sv_state* out) {
	if (!b || !out || out->size < sizeof(sv_state)) return SV_EINVAL;
	std::lock_guard<std::mutex> lk(b->m);
	std::memset(out, 0, sizeof *out);
	out->size = sizeof *out;
	if (b->phase == Phase::running && b->mod.api) b->mod.api->get_state(out);
	out->usb = b->phase == Phase::bootloader ? SV_USB_BOOTLOADER : (b->attached ? SV_USB_ATTACHED : SV_USB_DETACHED);
	out->resets = b->resets;
	return 0;
}

SV_API int32_t sv_get_flash(sv_board* b, uint8_t* out, int32_t max) {
	if (!b || !out || max < 0) return SV_EINVAL;
	std::lock_guard<std::mutex> lk(b->m);
	const int32_t n = std::min<int32_t>(max, SV_FLASH_SIZE);
	std::memcpy(out, b->shared->flash, static_cast<size_t>(n));
	return n;
}

SV_API int32_t sv_drive_path(sv_board* b, char* out, int32_t max) {
	if (!b) return SV_EINVAL;
	std::lock_guard<std::mutex> lk(b->m);
	if (b->phase != Phase::bootloader) return 0;
	const std::string p = b->drive.string();
	if (out && max > 0) {
		const size_t n = std::min<size_t>(p.size(), static_cast<size_t>(max - 1));
		std::memcpy(out, p.data(), n);
		out[n] = 0;
	}
	return static_cast<int32_t>(p.size());
}

SV_API int32_t sv_test(sv_board* b, int32_t what, int32_t arg) { SV_WITH_FW(b, b->mod.api->test(what, arg)); }

} // extern "C"
