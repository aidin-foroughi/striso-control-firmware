
#ifndef _AUX_JACK_H_
#define _AUX_JACK_H_

#include "config.h"

/* the jack's pins, ADC and thread (aux_jack.cpp; the detection is jack_detector.hpp) */
void aux_jack_init(void);
/* a new jack setting (config.jack2_mode_setting): the jack thread takes it at its next pass */
void aux_jack_switch_mode_setting(jack2_mode_t mode);

void aux_power_enable(void);
void aux_power_disable(void);

#endif
