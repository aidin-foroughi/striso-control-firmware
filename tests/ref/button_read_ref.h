/* The original key processing of button_read.c (button_read_ref.c), for the PC tests. */
#ifndef BUTTON_READ_REF_H_
#define BUTTON_READ_REF_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ref_config_t {
  int message_interval;
  int zero_offset;
};
extern struct ref_config_t ref_config;

struct ref_key_state {
  int32_t pres, velo, zero_offset, zero_time, zero_max, key_detect3;
  float fact;
  int status, timer;
  int32_t p;
};

// the hooks the test provides: msgSend (0: queued) and chThdSleep
int ref_send(int size, int *msg);
void ref_wait(int ticks);

void ref_init(void);
// the interrupt's writes: on for every button, the rest for a measured column's buttons
void ref_set_on(int b, int on);
void ref_set_inputs(int b, int on, int p, int s0, int s1, int s2);
void ref_set_c_force(int b, int32_t c_force);
void ref_set_zero_offset(int b, int32_t zero_offset);
void ref_get_state(int b, struct ref_key_state *k);
int ref_buttons_pressed(void);
int ref_col_pressed(int col);
// the thread's work for column note_id: the zero level pass at boot, the normal processing
void ref_boot_column(int note_id);
void ref_column(int note_id);

#ifdef __cplusplus
}
#endif

#endif
