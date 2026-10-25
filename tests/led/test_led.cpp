// The LEDs (src/drivers/led.cpp over the presenter, led_presenter.hpp) against the old led.c
// (tests/ref/led_ref.c), on the same stand-in HAL (tests/led/stubs): every PWM width, pin change,
// notification switch and OSAL lock is recorded, with the colour and the dimmed LED after each
// call. The new code must make the old code's calls from the same calls and times.
//
//   gamma     every input of the gamma table
//   updown    every up/down state (16^4)
//   dial      every dial angle in -1024..1024
//   blink     blinks and ticks around their end, and around the system time's wrap
//   random    20000 random calls of every kind, with the time going forward
//   mutex     each call takes and gives back the lock (the new code's one change)
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "doctest/doctest.h"
#include "led_presenter.hpp"

extern "C" {
#include "led.h"
extern int ref_last_color[3];
extern ioline_t ref_led_dimmed;
int ref_gamma8(int x);
void ref_led_init(void);
void ref_led_rgb(uint32_t rgb);
void ref_led_rgb3(int r, int g, int b);
void ref_led_rgb3_blink(int r, int g, int b, sysinterval_t time);
void ref_led_tick(void);
void ref_led_updown(uint32_t state);
uint32_t ref_led_updown_get(void);
void ref_led_updown_dial(int angle);
}

namespace {

struct Op {
	char kind; // P pwm width, S/L set/clear line, M line mode, s pwm start, n notification, K/k lock
	uint32_t a = 0, b = 0, c = 0;
	bool operator==(const Op&) const = default;
};
std::vector<Op> trace;
systime_t now = 0;
int mutex_depth = 0, mutex_misuse = 0, osal_failures = 0;
void rec(char k, uint32_t a = 0, uint32_t b = 0, uint32_t c = 0) { trace.push_back({k, a, b, c}); }

std::string show(const Op& o) {
	char s[64];
	std::snprintf(s, sizeof s, "%c(%u,%u,%u)", o.kind, o.a, o.b, o.c);
	return s;
}
std::string diff(const std::vector<Op>& a, const std::vector<Op>& b) {
	const std::size_t n = std::min(a.size(), b.size());
	for (std::size_t i = 0; i < n; i++) {
		if (!(a[i] == b[i])) return "op " + std::to_string(i) + ": old " + show(a[i]) + ", new " + show(b[i]);
	}
	if (a.size() != b.size()) return "lengths: old " + std::to_string(a.size()) + ", new " + std::to_string(b.size());
	return "";
}

} // namespace

extern "C" {
PWMDriver PWMD3 = {3, PWM_STOP, 4, 0, nullptr}, PWMD4 = {4, PWM_STOP, 4, 0, nullptr};
systime_t chVTGetSystemTime(void) { return now; }
void chMtxObjectInit(mutex_t* mp) { mp->locked = 0; }
void chMtxLock(mutex_t* mp) {
	mutex_misuse += mp->locked != 0;
	mp->locked = 1;
	mutex_depth++;
}
void chMtxUnlock(mutex_t* mp) {
	mutex_misuse += mp->locked != 1;
	mp->locked = 0;
	mutex_depth--;
}
void palSetLine(ioline_t l) { rec('S', l); }
void palClearLine(ioline_t l) { rec('L', l); }
void palSetLineMode(ioline_t l, uint32_t mode) { rec('M', l, mode); }
void pwmStart(PWMDriver* pwmp, const PWMConfig* config) {
	pwmp->config = config;
	pwmp->state = PWM_READY;
	rec('s', static_cast<uint32_t>(pwmp->id), config->frequency, config->period);
	for (const PWMChannelConfig& c : config->channels) rec('c', c.mode, c.callback != nullptr);
	rec('c', config->callback != nullptr, config->cr2, config->dier);
}
void pwmEnableChannel(PWMDriver* pwmp, pwmchannel_t channel, pwmcnt_t width) {
	pwmp->enabled |= 1u << channel;
	rec('P', static_cast<uint32_t>(pwmp->id), channel, width);
}
void pwmEnablePeriodicNotificationI(PWMDriver* pwmp) { rec('n', static_cast<uint32_t>(pwmp->id), 9, 1); }
void pwmDisablePeriodicNotificationI(PWMDriver* pwmp) { rec('n', static_cast<uint32_t>(pwmp->id), 9, 0); }
void pwmEnableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel) { rec('n', static_cast<uint32_t>(pwmp->id), channel, 1); }
void pwmDisableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel) { rec('n', static_cast<uint32_t>(pwmp->id), channel, 0); }
void osalSysLock(void) { rec('K'); }
void osalSysUnlock(void) { rec('k'); }
void osal_check(bool ok, const char* /*what*/) { osal_failures += !ok; }
}

namespace {

// one call, on either version; then the colour and the dimmed LED as the other files see them
struct Call {
	char what; // r rgb, 3 rgb3, b blink, t tick, u updown, d dial, g get
	uint32_t a = 0;
	int r = 0, g = 0, b = 0;
};
void call(bool old, const Call& c) {
	switch (c.what) {
	case 'r': old ? ref_led_rgb(c.a) : led_rgb(c.a); break;
	case '3': old ? ref_led_rgb3(c.r, c.g, c.b) : led_rgb3(c.r, c.g, c.b); break;
	case 'b': old ? ref_led_rgb3_blink(c.r, c.g, c.b, c.a) : led_rgb3_blink(c.r, c.g, c.b, c.a); break;
	case 't': old ? ref_led_tick() : led_tick(); break;
	case 'u': old ? ref_led_updown(c.a) : led_updown(c.a); break;
	case 'd': old ? ref_led_updown_dial(static_cast<int>(c.a)) : led_updown_dial(static_cast<int>(c.a)); break;
	case 'g': rec('g', old ? ref_led_updown_get() : led_updown_get()); break;
	default: break;
	}
	const int* col = old ? ref_last_color : last_color;
	rec('=', static_cast<uint32_t>(col[0]), static_cast<uint32_t>(col[1]), static_cast<uint32_t>(col[2]));
	rec('D', old ? ref_led_dimmed : led_dimmed);
}

// both versions from init through the calls, each with its time; the traces must be the same
std::string compare(const std::vector<std::pair<systime_t, Call>>& calls) {
	std::vector<Op> t[2];
	for (int old = 1; old >= 0; old--) {
		trace.clear();
		now = 0;
		old ? ref_led_init() : led_init();
		for (const auto& [time, c] : calls) {
			now = time;
			call(old != 0, c);
		}
		t[old] = trace;
	}
	return diff(t[1], t[0]);
}

} // namespace

TEST_CASE("gamma: every input, as the old table") {
	for (int x = 0; x <= 256; x++) {
		CAPTURE(x);
		CHECK(led::gamma8(x) == ref_gamma8(x));
	}
}

TEST_CASE("updown: every state, the same pins, notifications and dimmed LED") {
	std::vector<std::pair<systime_t, Call>> calls;
	for (uint32_t s = 0; s <= 0xffff; s++) calls.push_back({0, {'u', s}});
	for (uint32_t s : {0x10000u, 0xffff0000u, 0x12345678u, 0xffffffffu}) calls.push_back({0, {'u', s}});
	const std::string d = compare(calls);
	CHECK_MESSAGE(d == "", d);
	CHECK(osal_failures == 0);
}

TEST_CASE("dial: every angle in -1024..1024") {
	std::vector<std::pair<systime_t, Call>> calls;
	for (int a = -1024; a <= 1024; a++) {
		calls.push_back({0, {'d', static_cast<uint32_t>(a)}});
		calls.push_back({0, {'g'}});
	}
	const std::string d = compare(calls);
	CHECK_MESSAGE(d == "", d);
}

TEST_CASE("blink: its end, around the system time's wrap too") {
	for (systime_t start : {0u, 1000u, 0x7fffffffu, 0xfffff000u, 0xfffffff0u}) {
		CAPTURE(start);
		std::vector<std::pair<systime_t, Call>> calls = {{start, {'3', 0, 10, 20, 30}}, {start, {'b', 200, 90, 0, 0}}};
		for (systime_t dt = 0; dt < 400; dt += 7) calls.push_back({start + dt, {'t'}});
		calls.push_back({start + 500, {'b', 0x20, 0, 90, 0}});
		calls.push_back({start + 501, {'3', 0, 1, 2, 3}}); // a colour during a blink
		calls.push_back({start + 600, {'t'}});
		calls.push_back({start + 601, {'b', 0, 5, 5, 5}}); // a blink of no time
		calls.push_back({start + 601, {'t'}});
		calls.push_back({start + 602, {'t'}});
		const std::string d = compare(calls);
		CHECK_MESSAGE(d == "", d);
	}
}

TEST_CASE("random: 20000 calls of every kind") {
	for (unsigned seed = 1; seed <= 20; seed++) {
		CAPTURE(seed);
		std::mt19937 rng(seed);
		std::vector<std::pair<systime_t, Call>> calls;
		systime_t t = seed * 0x10000000u; // some runs cross the wrap
		for (int i = 0; i < 1000; i++) {
			t += rng() % 50;
			Call c;
			const int k = static_cast<int>(rng() % 7);
			const auto colour = [&] { return static_cast<int>(rng() % 3 ? rng() % 256 : rng() % 4); };
			const auto nibble = [&] { const unsigned v = rng() % 4; return v == 0 ? 0u : v == 1 ? 0xfu : 1u + rng() % 14; };
			c.what = "r3btudg"[k];
			c.r = colour();
			c.g = colour();
			c.b = colour();
			c.a = k == 0 ? rng() & 0xffffff : k == 2 ? rng() % 300 :
			    k == 4                               ? nibble() | nibble() << 4 | nibble() << 8 | nibble() << 12 :
			    k == 5                               ? static_cast<uint32_t>(static_cast<int>(rng() % 2049) - 1024) :
			                                           0;
			calls.emplace_back(t, c);
		}
		const std::string d = compare(calls);
		CHECK_MESSAGE(d == "", d);
	}
	CHECK(osal_failures == 0);
}

TEST_CASE("mutex: every call takes and gives back the lock") {
	mutex_depth = mutex_misuse = 0;
	led_init();
	led_rgb3(1, 2, 3);
	led_rgb3_blink(4, 5, 6, 10);
	led_tick();
	led_updown(0x1f0f);
	led_updown_dial(3);
	CHECK(mutex_depth == 0);
	CHECK(mutex_misuse == 0);
}
