/* The virtual board's HAL state: PAL lines, the LED PWM channels, the CMSIS registers the
 * firmware touches (DWT, PWR, RTC backup, UID). */
#include <string.h>

#include "hal.h"
#include "sv_host.h"

svh_board_io svh_io;
PWMDriver PWMD3, PWMD4;
sv_dwt_t sv_dwt;
sv_pwr_t sv_pwr;
sv_rtc_t* sv_rtc;
uint32_t* sv_uid;

static int line_index(ioline_t line) {
	const uint32_t i = PAL_PORT(line) * 16u + PAL_PAD(line);
	return i < sizeof svh_io.line ? (int)i : -1;
}

void halInit(void) {}

void palSetLine(ioline_t line) {
	const int i = line_index(line);
	if (i >= 0) svh_io.line[i] = 1;
}

void palClearLine(ioline_t line) {
	const int i = line_index(line);
	if (i >= 0) svh_io.line[i] = 0;
}

void palToggleLine(ioline_t line) {
	const int i = line_index(line);
	if (i >= 0) svh_io.line[i] ^= 1;
}

void palWriteLine(ioline_t line, int value) {
	const int i = line_index(line);
	if (i >= 0) svh_io.line[i] = value ? 1 : 0;
}

int palReadLine(ioline_t line) {
	const int i = line_index(line);
	return i >= 0 ? svh_io.line[i] : 0;
}

void palSetLineMode(ioline_t line, iomode_t mode) {
	const int i = line_index(line);
	if (i >= 0) svh_io.line_mode_set[i] = (uint8_t)mode;
}

void pwmStart(PWMDriver* pwmp, const PWMConfig* config) {
	pwmp->config = config;
	pwmp->period = config->period;
	pwmp->channels = PWM_CHANNELS;
	pwmp->state = PWM_READY;
}

void pwmEnableChannel(PWMDriver* pwmp, pwmchannel_t channel, pwmcnt_t width) {
	if (channel >= PWM_CHANNELS) return;
	pwmp->duty[channel] = width;
	pwmp->enabled |= 1u << channel;
}

void pwmDisableChannel(PWMDriver* pwmp, pwmchannel_t channel) {
	if (channel >= PWM_CHANNELS) return;
	pwmp->duty[channel] = 0;
	pwmp->enabled &= ~(1u << channel);
}

void pwmEnablePeriodicNotificationI(PWMDriver* pwmp) { pwmp->periodic_notification = true; }
void pwmDisablePeriodicNotificationI(PWMDriver* pwmp) { pwmp->periodic_notification = false; }

void pwmEnableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel) {
	if (channel < PWM_CHANNELS) pwmp->channel_notification[channel] = true;
}

void pwmDisableChannelNotificationI(PWMDriver* pwmp, pwmchannel_t channel) {
	if (channel < PWM_CHANNELS) pwmp->channel_notification[channel] = false;
}

sv_dwt_t* sv_dwt_update(void) {
	sv_dwt.CYCCNT = chSysGetRealtimeCounterX();
	return &sv_dwt;
}
