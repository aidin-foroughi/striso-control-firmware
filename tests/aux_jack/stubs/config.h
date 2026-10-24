#pragma once
/* Host stand-in for src/app/config.h: the jack modes and settings aux_jack.c uses. */
#include <stdbool.h>
#include <stdint.h>
typedef enum {
  JACK2_MODE_DISABLED = 0,
  JACK2_MODE_MIDI,
  JACK2_MODE_PEDAL_EXPRESSION,
  JACK2_MODE_PEDAL_SWITCH,
  JACK2_MODE_LINEIN,
  JACK2_MODE_AUTODETECT,
  JACK2_MODE_ERROR
} jack2_mode_t;
typedef struct {
  jack2_mode_t jack2_mode_setting;
  jack2_mode_t jack2_mode;
} config_t;
extern config_t config;
#define USE_MIDI_SERIAL
#define PLAYBACK_BUFFER_SIZE 64 /* codec.h */
