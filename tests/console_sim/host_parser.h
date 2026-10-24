/* C port of striso-live/packages/striso_core/lib/src/protocol.dart StreamParser (the host side of
 * the Striso binary stream + console text), to see what a host makes of the board's output. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
  int need, src, count, pos;
  uint8_t raw[15];
  char text[2048];
  int textlen;
  int messages, lines, truncated;
  int verbose;
  /* the messages seen, for checks */
  int msg_src[4096], msg_id[4096], msg_count[4096], msg_v[4096][7];
  char line[256][160];
} host_parser_t;

static inline int hp_decode14(int hi, int lo) {
  int v = ((hi & 0x7f) << 7) | (lo & 0x7f);
  return (v & 0x2000) ? v - 0x4000 : v;
}

static inline void hp_emit(host_parser_t *p) {
  int k = p->messages < 4096 ? p->messages : 4095;
  p->msg_src[k] = p->src;
  p->msg_id[k] = p->raw[0];
  p->msg_count[k] = p->count;
  for (int c = 0; c < p->count; c++) p->msg_v[k][c] = hp_decode14(p->raw[1 + 2 * c], p->raw[2 + 2 * c]);
  if (p->verbose) {
    printf("    host: message src=%d id=%d values=[", p->src, p->raw[0]);
    for (int c = 0; c < p->count; c++) printf("%s%d", c ? "," : "", p->msg_v[k][c]);
    printf("]\n");
  }
  p->messages++;
}

static inline void hp_push(host_parser_t *p, const uint8_t *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    uint8_t c = b[i];
    if (c & 0x80) {
      if (p->need > 0) {
        p->truncated++;
        if (p->verbose) printf("    host: truncated message (header byte inside a message)\n");
      }
      p->src = (c >> 3) & 0x0f;
      p->count = c & 0x07;
      p->need = 1 + 2 * p->count;
      p->pos = 0;
      continue;
    }
    if (p->need > 0) {
      p->raw[p->pos++] = c;
      if (--p->need == 0) hp_emit(p);
      continue;
    }
    if (c == 0x0a) {
      p->text[p->textlen] = 0;
      if (p->lines < 256) snprintf(p->line[p->lines], sizeof p->line[0], "%s", p->text);
      if (p->verbose) printf("    host: line \"%s\"\n", p->text);
      p->lines++;
      p->textlen = 0;
    } else if (c != 0x0d) {
      if (p->textlen < 2047) p->text[p->textlen++] = (char)c;
    }
  }
}
