/* D4: the aux jack's line-in mode (src/drivers/codec_aic3x_linein.h) against a model of the
 * TLV320AIC3104's analog input path, from its datasheet's page 0 register map:
 *
 *   LINE2L -> reg 17 D7-D4 (level into the left ADC PGA mix, 1111 = not connected)
 *          -> the left ADC PGA: powered with the left ADC channel (reg 19 D2), unmuted (reg 15 D7 = 0)
 *          -> reg 81 D7 (PGA_L routed to LEFT_LOP/M)
 *   LINE2R -> reg 18 D3-D0 -> the right ADC PGA (reg 22 D2, reg 16 D7) -> reg 91 D7 -> RIGHT_LOP/M
 *
 * Starting from the reset values, the "on" writes must open both paths and the "off" writes must
 * close them again and power the ADC channels down (their reset state). Before the fix line-in
 * routed LINE2 into a PGA that was never powered up nor unmuted: silent. */
#include <stdbool.h>
#include <stdio.h>

#include "codec_aic3x_linein.h"

static uint8_t reg[128];

static void reset_values(void) {
	for (int i = 0; i < 128; i++) reg[i] = 0;
	reg[LADC_VOL] = 0x80; /* reg 15/16: ADC PGA muted, 0 dB */
	reg[RADC_VOL] = 0x80;
	reg[LINE2L_2_LADC_CTRL] = 0xFF; /* reg 17/18: nothing connected */
	reg[LINE2R_2_RADC_CTRL] = 0xFF;
	reg[LINE1L_2_LADC_CTRL] = 0x78; /* reg 19/22: LINE1 not connected, ADC channel powered down */
	reg[LINE1R_2_RADC_CTRL] = 0x78;
	/* reg 81/91: PGA not routed to the line outputs (0x00) */
}
static void apply(const uint8_t (*w)[2], int n) {
	for (int i = 0; i < n; i++) reg[w[i][0]] = w[i][1];
}
static bool left_path(void) {
	return (reg[LINE2L_2_LADC_CTRL] >> 4) != 0xF && (reg[LINE1L_2_LADC_CTRL] & LADC_PWR_ON) &&
	    !(reg[LADC_VOL] & MUTE_ON) && (reg[PGAL_2_LLOPM_VOL] & ROUTE_ON);
}
static bool right_path(void) {
	return (reg[LINE2R_2_RADC_CTRL] & 0xF) != 0xF && (reg[LINE1R_2_RADC_CTRL] & RADC_PWR_ON) &&
	    !(reg[RADC_VOL] & MUTE_ON) && (reg[PGAR_2_RLOPM_VOL] & ROUTE_ON);
}
static bool adc_powered(void) { return (reg[LINE1L_2_LADC_CTRL] & LADC_PWR_ON) || (reg[LINE1R_2_RADC_CTRL] & RADC_PWR_ON); }
/* LINE1 stays unconnected (nothing is wired to it), the LINE2 levels are 0 dB, the PGA gain 0 dB */
static bool levels_ok(void) {
	return ((reg[LINE1L_2_LADC_CTRL] >> 3) & 0xF) == 0xF && ((reg[LINE1R_2_RADC_CTRL] >> 3) & 0xF) == 0xF &&
	    (reg[LINE2L_2_LADC_CTRL] >> 4) == 0 && (reg[LINE2R_2_RADC_CTRL] & 0xF) == 0 &&
	    (reg[LADC_VOL] & 0x7F) == 0 && (reg[RADC_VOL] & 0x7F) == 0;
}

static int failures;
static void check(const char* name, bool ok) {
	failures += !ok;
	printf("%-4s %s\n", ok ? "ok" : "FAIL", name);
}

int main(void) {
	const int non = sizeof aic3x_linein_on / sizeof aic3x_linein_on[0];
	const int noff = sizeof aic3x_linein_off / sizeof aic3x_linein_off[0];
	reset_values();
	check("before line-in: no path from LINE2 to the line outputs", !left_path() && !right_path());
	apply(aic3x_linein_on, non);
	check("line-in on: LINE2L reaches LEFT_LOP (PGA powered, unmuted, routed)", left_path());
	check("line-in on: LINE2R reaches RIGHT_LOP (PGA powered, unmuted, routed)", right_path());
	check("line-in on: 0 dB levels, LINE1 still not connected", levels_ok());
	apply(aic3x_linein_off, noff);
	check("line-in off: both paths closed", !left_path() && !right_path());
	check("line-in off: the ADC channels powered down again", !adc_powered());
	apply(aic3x_linein_on, non);
	check("line-in on again", left_path() && right_path());
	printf(failures ? "codec line-in: FAILED\n" : "codec line-in: all passed\n");
	return failures ? 1 : 0;
}
