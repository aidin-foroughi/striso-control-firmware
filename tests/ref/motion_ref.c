/*
 * The math of motionsensor.c's LSM6DSL reader loop as it was at cc47a4d, before the MotionFilter
 * (src/drivers/motion_filter.hpp): the reference that tests/motion compares the filter with, bit
 * for bit, on the PC and on the board's CPU (QEMU). The loop body after the sensor read is copied
 * unchanged; around it only what the test needs: the configuration, the message queue and the
 * synth's controls as hooks (motion_ref.h), and one call per reading (ref_motion_reading).
 */
#include <math.h>
#include <stdint.h>

#include "motion_ref.h"

#define ACC_FS 2
#define pow2(x) ((x)*(x))
#define USE_INTERNAL_SYNTH
#define ID_ACCEL 3
#define ID_ACCEL_STREAM (ID_ACCEL | 0x10)
#define config ref_motion_config
#define synth_interface ref_motion_synth
#define event_send ref_motion_send

struct ref_motion_config_t ref_motion_config;
struct ref_motion_synth_t ref_motion_synth;
static int msg[9] = {ID_ACCEL, 8};
static int stream_count = 0;

void ref_motion_reset(void) {
  msg[0] = ID_ACCEL;
  msg[1] = 8;
  stream_count = 0;
}

void ref_motion_reading(const int16_t motion[6]) {
    int16_t ax, ay, az, gx, gy, gz;

    ax = -motion[3]; gx = -motion[0];
    ay = -motion[4]; gy = -motion[1];
    az = -motion[5]; gz = -motion[2];

    float acc_x = ((float)ax)*(ACC_FS/32768.0f);
    float acc_y = ((float)ay)*(ACC_FS/32768.0f);
    float acc_z = ((float)az)*(ACC_FS/32768.0f);
    float acc_abs = sqrtf(pow2(acc_x) + pow2(acc_y) + pow2(acc_z));
    if (acc_abs>0.001) {
      acc_x /= acc_abs;
      acc_y /= acc_abs;
      acc_z /= acc_abs;
    }

    if (config.send_motion_interval) {
      if (config.send_motion_interval != 127) {
        // 127 means only internal synth
        // acceleration in message has maximum range 8g (16g for acc_abs)
        msg[2] = (ax*ACC_FS/8)>>2;
        msg[3] = (ay*ACC_FS/8)>>2;
        msg[4] = (az*ACC_FS/8)>>2;
        msg[5] = ((int16_t)(acc_abs * 32768.0f/8.0f))>>2;
        msg[6] = gx>>2;
        msg[7] = gy>>2;
        msg[8] = gz>>2;
        event_send(msg[0], msg[1], 7, &msg[2]);
      }
      else if (config.send_usb_bulk && ++stream_count >= 4) {
        // internal synth only, but the USB stream is on (e.g. the Striso Live app): send every
        // 4th reading (~60 per second) as stream-only motion data, no MIDI side effects
        stream_count = 0;
        msg[0] = ID_ACCEL_STREAM;
        msg[2] = (ax*ACC_FS/8)>>2;
        msg[3] = (ay*ACC_FS/8)>>2;
        msg[4] = (az*ACC_FS/8)>>2;
        msg[5] = ((int16_t)(acc_abs * 32768.0f/8.0f))>>2;
        msg[6] = gx>>2;
        msg[7] = gy>>2;
        msg[8] = gz>>2;
        event_send(msg[0], msg[1], 7, &msg[2]);
        msg[0] = ID_ACCEL;
      }

#ifdef USE_INTERNAL_SYNTH
      float rot_x = ((float)gx)*(1.0f/32768.0f);
      float rot_y = ((float)gy)*(1.0f/32768.0f);
      float rot_z = ((float)gz)*(1.0f/32768.0f);

      *(synth_interface.acc_abs) = acc_abs;
      *(synth_interface.acc_x) = acc_x;
      *(synth_interface.acc_y) = acc_y;
      *(synth_interface.acc_z) = acc_z;
      *(synth_interface.rot_x) = rot_x;
      *(synth_interface.rot_y) = rot_y;
      *(synth_interface.rot_z) = rot_z;
#endif
    }
}
