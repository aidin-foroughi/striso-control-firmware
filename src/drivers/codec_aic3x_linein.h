/*
 * The TLV320AIC3104's register writes for the aux jack's line-in mode (codec_linein_enable and
 * codec_linein_disable, codec_tlv320aic3x_SAI.c): {page 0 register, value}, written in order.
 * A table so the host test (tests/test_codec_linein.c) can check the analog path against the
 * datasheet's register map.
 */
#ifndef CODEC_AIC3X_LINEIN_H
#define CODEC_AIC3X_LINEIN_H

#include <stdint.h>

#include "codec_tlv320aic3x.h"

// The PGA belongs to the ADC channel: it carries nothing unless that channel is powered up (reg
// 19/22 D2; reset: powered down) and the PGA unmuted (reg 15/16 D7; reset: muted).
static const uint8_t aic3x_linein_on[][2] = {
    {LINE2L_2_LADC_CTRL, 0x0F}, // reg 17: LINE2L into the left ADC PGA mix at 0 dB
    {LINE2R_2_RADC_CTRL, 0xF0}, // reg 18: LINE2R into the right ADC PGA mix at 0 dB
    {LINE1L_2_LADC_CTRL, 0x7C}, // reg 19: LINE1L not connected, left ADC channel powered up
    {LINE1R_2_RADC_CTRL, 0x7C}, // reg 22: LINE1R not connected, right ADC channel powered up
    {LADC_VOL, 0x00},           // reg 15: left ADC PGA unmuted, 0 dB
    {RADC_VOL, 0x00},           // reg 16: right ADC PGA unmuted, 0 dB
    {PGAL_2_LLOPM_VOL, 0x80},   // reg 81: PGA_L to LEFT_LOP, 0 dB
    {PGAR_2_RLOPM_VOL, 0x80},   // reg 91: PGA_R to RIGHT_LOP, 0 dB
};

// back to the reset state: unrouted, muted, powered down, not connected
static const uint8_t aic3x_linein_off[][2] = {
    {PGAL_2_LLOPM_VOL, 0x00},   // reg 81: not routed
    {PGAR_2_RLOPM_VOL, 0x00},   // reg 91: not routed
    {LADC_VOL, 0x80},           // reg 15: muted
    {RADC_VOL, 0x80},           // reg 16: muted
    {LINE1L_2_LADC_CTRL, 0x78}, // reg 19: left ADC channel powered down
    {LINE1R_2_RADC_CTRL, 0x78}, // reg 22: right ADC channel powered down
    {LINE2L_2_LADC_CTRL, 0xFF}, // reg 17: not connected
    {LINE2R_2_RADC_CTRL, 0xFF}, // reg 18: not connected
};

#endif
