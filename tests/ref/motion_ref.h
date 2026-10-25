/* The hooks of tests/ref/motion_ref.c (the old motion math): its configuration, the synth's
 * controls it writes and the message queue it sends to (the test provides ref_motion_send). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ref_motion_config_t {
  int send_motion_interval;
  bool send_usb_bulk;
};
struct ref_motion_synth_t {
  float *acc_abs, *acc_x, *acc_y, *acc_z, *rot_x, *rot_y, *rot_z;
};
extern struct ref_motion_config_t ref_motion_config;
extern struct ref_motion_synth_t ref_motion_synth;
bool ref_motion_send(int src, int id, int n, const int *v);
void ref_motion_reset(void);
void ref_motion_reading(const int16_t motion[6]);
#ifdef __cplusplus
}
#endif
