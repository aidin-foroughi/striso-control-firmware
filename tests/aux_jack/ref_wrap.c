/* The old aux_jack.c (tests/ref/aux_jack_ref.c, as it was at cc47a4d) in the test, with its
 * public names prefixed ref_ so it links next to the new aux_jack.cpp, and a call that runs its
 * thread (test_jack_detector.cpp stops it from the ADC stub with a longjmp). */
#include "config.h"
#define aux_power ref_aux_power
#define jack_detect ref_jack_detect
#define short_circuit_count ref_short_circuit_count
#define last_pedal_exp ref_last_pedal_exp
#define last_pedal1 ref_last_pedal1
#define last_pedal2 ref_last_pedal2
#define last_pedal3 ref_last_pedal3
#define adccfg3 ref_adccfg3
#define adcgrpcfg3 ref_adcgrpcfg3
#define aux_power_enable ref_aux_power_enable
#define aux_power_disable ref_aux_power_disable
#define aux_jack_init ref_aux_jack_init
#define aux_jack_switch_mode ref_aux_jack_switch_mode
#define aux_jack_switch_mode_setting ref_aux_jack_switch_mode_setting
void ref_aux_power_enable(void);
void ref_aux_power_disable(void);
void ref_aux_jack_init(void);
void ref_aux_jack_switch_mode(jack2_mode_t mode);
void ref_aux_jack_switch_mode_setting(jack2_mode_t mode);
#include "../ref/aux_jack_ref.c"

void ref_aux_jack_run(void) { AuxJack(NULL); }
void ref_aux_jack_reset(void) {
  short_circuit_count = 0;
  jack_detect = 31;
  last_pedal1 = 0;
  last_pedal2 = 1;
}
