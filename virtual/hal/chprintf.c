/*
    ChibiOS - Copyright (C) 2006..2018 Giovanni Di Sirio

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/
/*
   Concepts and parts of this file have been contributed by Fabio Utzig,
   chvprintf() added by Brent Roman.
*/
/* The virtual board's chprintf: ChibiOS' os/hal/lib/streams/chprintf.c (the submodule's version,
 * CHPRINTF_USE_FLOAT off as in cfg/), with `long` as the board's 32 bits (int32_t/uint32_t):
 * the firmware passes uint32_t for %lu/%lx. As there, `long % unsigned` is unsigned arithmetic (the
 * same width), so values past 2^31 print right. Hex digits are upper case, as on the board. */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>

#include "chprintf.h"
#include "memstreams.h"

#define MAX_FILLER 11

typedef int32_t long32;

static char* long_to_string_with_divisor(char* p, long32 num, unsigned radix, long32 divisor) {
	int i;
	char* q;
	long32 l, ll;

	l = num;
	if (divisor == 0) {
		ll = num;
	} else {
		ll = divisor;
	}

	q = p + MAX_FILLER;
	do {
		i = (int)(l % radix); /* long % unsigned: unsigned arithmetic on the board's 32-bit long */
		i += '0';
		if (i > '9') {
			i += 'A' - '0' - 10;
		}
		*--q = (char)i;
		l /= radix;
	} while ((ll /= radix) != 0);

	i = (int)(p + MAX_FILLER - q);
	do
		*p++ = *q++;
	while (--i);

	return p;
}

static char* ch_ltoa(char* p, long32 num, unsigned radix) { return long_to_string_with_divisor(p, num, radix, 0); }

int chvprintf(BaseSequentialStream* chp, const char* fmt, va_list ap) {
	char *p, *s, c, filler;
	int i, precision, width;
	int n = 0;
	bool is_long, left_align, do_sign;
	long32 l;
	char tmpbuf[MAX_FILLER + 1];

	while (true) {
		c = *fmt++;
		if (c == 0) {
			return n;
		}

		if (c != '%') {
			streamPut(chp, (uint8_t)c);
			n++;
			continue;
		}

		p = tmpbuf;
		s = tmpbuf;

		/* Alignment mode.*/
		left_align = false;
		if (*fmt == '-') {
			fmt++;
			left_align = true;
		}

		/* Sign mode.*/
		do_sign = false;
		if (*fmt == '+') {
			fmt++;
			do_sign = true;
		}

		/* Filler mode.*/
		filler = ' ';
		if (*fmt == '0') {
			fmt++;
			filler = '0';
		}

		/* Width modifier.*/
		if (*fmt == '*') {
			width = va_arg(ap, int);
			++fmt;
			c = *fmt++;
		} else {
			width = 0;
			while (true) {
				c = *fmt++;
				if (c == 0) {
					return n;
				}
				if (c >= '0' && c <= '9') {
					c -= '0';
					width = width * 10 + c;
				} else {
					break;
				}
			}
		}

		/* Precision modifier.*/
		precision = 0;
		if (c == '.') {
			c = *fmt++;
			if (c == 0) {
				return n;
			}
			if (c == '*') {
				precision = va_arg(ap, int);
				c = *fmt++;
			} else {
				while (c >= '0' && c <= '9') {
					c -= '0';
					precision = precision * 10 + c;
					c = *fmt++;
					if (c == 0) {
						return n;
					}
				}
			}
		}

		/* Long modifier.*/
		if (c == 'l' || c == 'L') {
			is_long = true;
			c = *fmt++;
			if (c == 0) {
				return n;
			}
		} else {
			is_long = (c >= 'A') && (c <= 'Z');
		}

		/* Command decoding.*/
		switch (c) {
		case 'c':
			filler = ' ';
			*p++ = (char)va_arg(ap, int);
			break;
		case 's':
			filler = ' ';
			if ((s = va_arg(ap, char*)) == 0) {
				s = (char*)"(null)";
			}
			if (precision == 0) {
				precision = 32767;
			}
			for (p = s; *p && (--precision >= 0); p++)
				;
			break;
		case 'D':
		case 'd':
		case 'I':
		case 'i':
			/* 'l': the board's long is 32 bits (va_arg of an int reads the low half of a host long) */
			l = (long32)va_arg(ap, int);
			(void)is_long;
			if (l < 0) {
				*p++ = '-';
				l = -l;
			} else if (do_sign) {
				*p++ = '+';
			}
			p = ch_ltoa(p, l, 10);
			break;
		case 'X':
		case 'x':
		case 'P':
		case 'p':
			c = 16;
			goto unsigned_common;
		case 'U':
		case 'u':
			c = 10;
			goto unsigned_common;
		case 'O':
		case 'o':
			c = 8;
		unsigned_common:
			l = (long32)va_arg(ap, unsigned int);
			p = ch_ltoa(p, l, (unsigned)c);
			break;
		default:
			*p++ = c;
			break;
		}
		i = (int)(p - s);
		if ((width -= i) < 0) {
			width = 0;
		}
		if (left_align == false) {
			width = -width;
		}
		if (width < 0) {
			if ((*s == '-' || *s == '+') && filler == '0') {
				streamPut(chp, (uint8_t)*s++);
				n++;
				i--;
			}
			do {
				streamPut(chp, (uint8_t)filler);
				n++;
			} while (++width != 0);
		}
		while (--i >= 0) {
			streamPut(chp, (uint8_t)*s++);
			n++;
		}

		while (width) {
			streamPut(chp, (uint8_t)filler);
			n++;
			width--;
		}
	}
}

int chprintf(BaseSequentialStream* chp, const char* fmt, ...) {
	va_list ap;
	int formatted_bytes;

	va_start(ap, fmt);
	formatted_bytes = chvprintf(chp, fmt, ap);
	va_end(ap);

	return formatted_bytes;
}

/* chsnprintf and chvsnprintf as chprintf.c's, on ChibiOS' memory stream (memstreams.c) */
int chvsnprintf(char* str, size_t size, const char* fmt, va_list ap) {
	MemoryStream ms;
	const size_t size_wo_nul = size > 0 ? size - 1 : 0;
	msObjectInit(&ms, (uint8_t*)str, size_wo_nul, 0);
	const int retval = chvprintf((BaseSequentialStream*)(void*)&ms, fmt, ap);
	if (ms.eos < size) str[ms.eos] = 0;
	return retval;
}

int chsnprintf(char* str, size_t size, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	const int retval = chvsnprintf(str, size, fmt, ap);
	va_end(ap);
	return retval;
}
