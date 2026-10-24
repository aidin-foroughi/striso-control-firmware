/* usb_audio_core.h across the wrap of the free-running 32-bit frame counters (2^32 frames = 24.85 h
   at 48 kHz): writer and reader at the nominal rate, counters started just below the wrap. */
#include <stdio.h>
#include "usb_audio_core.h"
static ua_core_t c;
static uint8_t pkt[UA_PACKET_MAX];
int main(void) {
	ua_init(&c);
	ua_stats_reset(&c);
	c.w = 0xFFFFFFFFu - 20u * UA_BLOCK + 1u; /* w stays a multiple of UA_BLOCK */
	c.w -= c.w % UA_BLOCK;
	ua_start(&c);
	double tw = 0, tr = 0;
	long frames = 0;
	for (int ms = 0; ms < 2000; ms++) { /* 2 s, 48 frames/ms in, one packet per ms out */
		tw += 48.0;
		while (tw >= UA_BLOCK) {
			ua_write_commit(&c, 0);
			tw -= UA_BLOCK;
		}
		tr += 1.0;
		size_t n = ua_fill(&c, pkt, 3);
		frames += (long)(n / 6);
	}
	printf("w %u r %u level %u; packets %u underruns %u overruns %u primes %u fast starts %u\n",
	    c.w, c.r, c.w - c.r, c.packets, c.underruns, c.overruns, c.primes, c.fast_starts);
	return (c.underruns || c.overruns) ? 1 : 0;
}
