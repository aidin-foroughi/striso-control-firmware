/*
 * The key processing of button_read.c as it was before the KeyScanner (play-trumpet, 3d6f498),
 * for the PC: the reference that tests/test_key_scanner.cpp compares the KeyScanner with, bit
 * for bit. The functions and the thread's per column work are copied unchanged; around them only
 * what the PC needs: the configuration, msgSend and chThdSleep as hooks, and the thread's loop
 * as one call per column (ref_column, ref_boot_column). Compiled with -fwrapv: the board's int32
 * products wrap near full pressure (the tests keep the float conversions in range, where the PC
 * and the board agree).
 */
#include <stdbool.h>
#include <stdint.h>

#include "button_read_ref.h"

#define DETECT_STUCK_NOTES
#define ID_DIS 0
#define CALIB_FORCE  ((1<<18)/64 + 1)

#define config ref_config
struct ref_config_t ref_config = {1, 0};
#define N_BUTTONS 68

#define INTERNAL_ONE (1<<24)
#define ADC_BITS 12
#define ADC_MAX ((1<<ADC_BITS)-1)
#define ADCFACT (INTERNAL_ONE >> ADC_BITS)  // factor from adc sample to INTERNAL_ONE
#define VELOFACT 32      // extra factor for velocity
#define MSGFACT (1<<11)  // factor from 14 bit message to INTERNAL_ONE
#define MSGFACT_VELO (MSGFACT/VELOFACT)
#define FILT 8  // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define FILTV 8 // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define ZERO_LEVEL_FACT 300 / 256  // safety factor for zero level. Without brackets so multiplication goes before division
#define ZERO_LEVEL_TIME 500
#define ZERO_LEVEL_MAX_PRES (INTERNAL_ONE/32)
#define ZERO_LEVEL_MAX_VELO 500
#define COMMON_CHANNEL_FILT 0.5
#define KEY_DETECT 64   // key_detect threshold
#define KEY_DETECT2 (256-KEY_DETECT)  // additional threshold when another key in the column is pressed
#define KEY_DETECT3 (320-KEY_DETECT2-KEY_DETECT) // additional threshold when 3 or 4 corners are pressed
#define MIN_MEASURES 4 // minimum notes to measure, must be >= 2
#define MULTISAMPLE 4  // multisampling of pressure, also hardcoded in some places

#define INTEGRATED_PRES_TRESHOLD (INTERNAL_ONE/8)
#define SENDFACT    config.message_interval

enum button_status {
  OFF = 0,
  STARTING = 1,
  ON = 2,
};

typedef struct struct_button button_t;
struct struct_button {
  int32_t on;
  int32_t p;
  int32_t s0;
  int32_t s1;
  int32_t s2;
  int32_t pres;
  int32_t velo;
  int32_t c_force;
  int32_t c_breakpoint;
  int32_t c_force2;
  int32_t zero_offset;
  int32_t zero_time;
  int32_t zero_max;
  int32_t key_detect3;
  float fact;
  enum button_status status;
  int timer;
  int but_id;
  int src_id;
};

static button_t buttons[N_BUTTONS];
static int buttons_pressed[2] = {0};
static int col_pressed[2][17] = {0};

static int msgSend(int size, int* msg) { return ref_send(size, msg); }
static void chThdSleep(int ticks) { ref_wait(ticks); }

// Schlick power function, approximation of power function
float powf_schlick(const float a, const float b) {
  return (a / (b - a * b + a));
}

/*
 * Second order Kalman like filter with fast signal end conditions
 */
void update_and_filter(int32_t* s, int32_t* v, int32_t s_new) {
  int32_t old_s = *s;
  *s = ((FILT-1) * (old_s + *v) + s_new) / FILT;
  if (*s >= INTERNAL_ONE) {
    *s = INTERNAL_ONE - 1;
    *v = 0;
  } else {
    *v = ((FILTV-1) * (*v) + (*s - old_s)) / FILTV;
    if (*v >= (INTERNAL_ONE/VELOFACT)) {
      *v = (INTERNAL_ONE/VELOFACT) - 1;
    } else if (*v <= -(INTERNAL_ONE/VELOFACT)) {
      *v = -(INTERNAL_ONE/VELOFACT) + 1;
    }
    if (*s < 0) {
      *s = 0;
    }
  }
}

int32_t linearize(int32_t s) {
#ifdef CALIBRATION_MODE
  /* keep linear voltage for calibration */
  return ADCFACT / MULTISAMPLE * s;
#else
  /* convert adc value to force */
  return (ADCFACT>>6) * s/((MULTISAMPLE*4095)-s+1);
#endif // CALIBRATION_MODE
}

int32_t calibrate(int32_t s, button_t* but) {
#ifdef CALIBRATION_MODE
  return s;
#endif
  // c is the normalisation value for the force
  //    2^18   * 2^12 / 2^12 * ADCFACT/2^6 / c
  // s = (but->c_force * (4095-s)/(s+1)) * (ADCFACT>>6);
  s = (but->c_force * s) * but->fact;
  #ifdef BREAKPOINT_CALIBRATION
  // breakpoint calibration
  if (s > but->c_breakpoint) {
    s += but->c_force2 * ((s - but->c_breakpoint)>>8);
  }
  #endif // BREAKPOINT_CALIBRATION
  return s;
}

#define max(x,y) ((x)>(y)?(x):(y))
#define min(x,y) ((x)<(y)?(x):(y))

void update_button(button_t* but) {
  int but_id = but->but_id;
  int32_t s_new;
  int msg[8];
  msg[0] = but->src_id;

  int key_detect2 = KEY_DETECT2 * (col_pressed[but->src_id][but_id % 17] - (but->status != OFF) >= 1);

  if (but->on > KEY_DETECT + key_detect2 + but->key_detect3) {

    s_new = calibrate(linearize(but->p), but);
    // four corner correction algoritm
    if (but->key_detect3) {
      for (button_t* but2 = &buttons[but_id % 17]; but2 < &buttons[N_BUTTONS]; but2 = &but2[17]) {
        if (but != but2 && but2->key_detect3 && but2->status) {
          int32_t s_new2 = calibrate(linearize(but2->p), but2);
          if (s_new2 > s_new) {
            s_new -= (s_new2 - s_new) / 2;
          } else {
            s_new -= (s_new2 - s_new);
          }
          break;
        }
      }
    }
    update_and_filter(&but->pres, &but->velo, s_new);

#ifdef DETECT_STUCK_NOTES
    // adjust zero pressure level dynamically
    if (but->pres < ZERO_LEVEL_MAX_PRES
        && but->velo < ZERO_LEVEL_MAX_VELO
        && but->velo > -ZERO_LEVEL_MAX_VELO) {
      if (but->pres > but->zero_max) but->zero_max = but->pres;
      but->zero_time++;
      if (but->zero_time > ZERO_LEVEL_TIME) {
        but->zero_time = 0;
#ifdef DETECT_STUCK_NOTES_DECREASE
        but->zero_offset = but->zero_max * ZERO_LEVEL_FACT;
#else
        but->zero_offset = max(but->zero_offset, but->zero_max * ZERO_LEVEL_FACT);
#endif
      }
    } else {
      but->zero_time = 0;
      but->zero_max = 0;
    }
#endif

    // if button is off start integration timer
    if (but->status == OFF) {
      but->status = STARTING;
      but->timer = INTEGRATED_PRES_TRESHOLD;
      col_pressed[but->src_id][but_id % 17]++;
    }
    // if button is in start integration reduce timer
    if (but->status == STARTING && but->pres > (config.zero_offset + but->zero_offset + MSGFACT)) {
      but->timer -= (but->pres - but->zero_offset - MSGFACT);
    }
    // note off if .pres is too low even though .on is high enough
    else if (but->status == ON && but->pres < (config.zero_offset / 2 + but->zero_offset + MSGFACT)) {
      but->status = STARTING;
      buttons_pressed[but->src_id]--;
      but->timer = INTEGRATED_PRES_TRESHOLD;

      msg[1] = but_id;
      msg[2] = 0;
      msg[3] = min(but->velo, 0) / MSGFACT_VELO;
      msg[4] = 0;
      msg[5] = 0;
      while (msgSend(6, msg)) { // note off messages are more important so keep trying
        chThdSleep(1);
      }
    }
    // if integration is succesful and interval is ready send note message
    if (--but->timer <= 0) {
      if (but->status != ON) {
        but->status = ON;
        buttons_pressed[but->src_id]++;
      }

      // calculate values from signals
      #define CENTERTEND 0.02f
      int32_t but_x, but_y;
      int32_t s0 = calibrate(linearize(but->s0), but);
      int32_t s1 = calibrate(linearize(but->s1), but);
      int32_t s2 = calibrate(linearize(but->s2), but);
      // m = max(s0, s1, s2)
      int32_t m = s0;
      if (s1 > m) m = s1;
      if (s2 > m) m = s2;
      if (m > 0) {
          float mf = ((float)m)/INTERNAL_ONE;
          float fact = 1.0f/(mf + CENTERTEND/mf - CENTERTEND);
          but_x = (s2 - s0) * fact;
          but_y = ((s0 + s2) / 2 - s1) * fact;
      } else {
          but_x = 0;
          but_y = 0;
      }

      msg[1] = but_id;
      msg[2] = but->pres / MSGFACT;
      msg[3] = but->velo / MSGFACT_VELO; // but->on;// s_new / MSGFACT; //
      msg[4] = but_x / MSGFACT;
      msg[5] = but_y / MSGFACT;
      msgSend(6, msg);
      but->timer = (buttons_pressed[0] + buttons_pressed[1]) * SENDFACT;
    }
  }
  else if (but->status) {
    if (but->status == ON) {
      msg[1] = but_id;
      msg[2] = 0;
      msg[3] = min(but->velo, 0) / MSGFACT_VELO;
      msg[4] = 0;
      msg[5] = 0;
      while (msgSend(6, msg)) { // note off messages are more important so keep trying
        chThdSleep(1);
      }
      buttons_pressed[but->src_id]--;
    }
    but->status = OFF;
    but->p = 0;
    col_pressed[but->src_id][but_id % 17]--;
    // reset filter
    but->pres = 0;
    but->velo = 0;
    but->zero_time = 0;
  } else {
    but->p = 0;
#ifdef DETECT_STUCK_NOTES_DECREASE
    but->zero_time++;
    if (but->zero_time == ZERO_LEVEL_TIME) {
      but->zero_offset = 0;
    }
#endif
  }
  but->fact = 1.0f;
  but->key_detect3 = 0;
}

void ref_init(void) {
  for (int n=0; n<N_BUTTONS; n++) {
    button_t zero = {0};
    buttons[n] = zero;
    buttons[n].but_id = n;
    buttons[n].src_id = ID_DIS;
    buttons[n].c_force = CALIB_FORCE;
    buttons[n].key_detect3 = 0;
    buttons[n].pres = 0;
    buttons[n].velo = 0;
    buttons[n].zero_offset = 0;
    buttons[n].zero_time = 0;
    buttons[n].zero_max = 0;
    buttons[n].fact = 1.0f;
    buttons[n].c_breakpoint = INT32_MAX;
  }
  // disable not existing buttons
  buttons[52].c_force = 0;
  buttons[54].c_force = 0;
  buttons[57].c_force = 0;
  buttons[59].c_force = 0;
  buttons[61].c_force = 0;
  buttons[64].c_force = 0;
  buttons[66].c_force = 0;
  buttons_pressed[0] = buttons_pressed[1] = 0;
  for (int c = 0; c < 17; c++) col_pressed[0][c] = col_pressed[1][c] = 0;
}

void ref_set_inputs(int b, int on, int p, int s0, int s1, int s2) {
  buttons[b].on = on;
  buttons[b].p = p;
  buttons[b].s0 = s0;
  buttons[b].s1 = s1;
  buttons[b].s2 = s2;
}

void ref_set_on(int b, int on) { buttons[b].on = on; }

void ref_set_c_force(int b, int32_t c_force) { buttons[b].c_force = c_force; }
void ref_set_zero_offset(int b, int32_t zero_offset) { buttons[b].zero_offset = zero_offset; }

void ref_get_state(int b, struct ref_key_state *k) {
  k->pres = buttons[b].pres;
  k->velo = buttons[b].velo;
  k->zero_offset = buttons[b].zero_offset;
  k->zero_time = buttons[b].zero_time;
  k->zero_max = buttons[b].zero_max;
  k->key_detect3 = buttons[b].key_detect3;
  k->fact = buttons[b].fact;
  k->status = (int)buttons[b].status;
  k->timer = buttons[b].timer;
  k->p = buttons[b].p;
}

int ref_buttons_pressed(void) { return buttons_pressed[0]; }
int ref_col_pressed(int col) { return col_pressed[0][col]; }

void ref_boot_column(int note_id) {
      for (int n = 0; n < 4; n++) {
        button_t* but = &buttons[note_id + n * 17];
        if (but->on > KEY_DETECT) {
          int32_t s_new = calibrate(linearize(but->p), but);
          update_and_filter(&but->pres, &but->velo, s_new);
          s_new = but->pres * ZERO_LEVEL_FACT;
          if (s_new > but->zero_offset) {
            but->zero_offset = s_new;
          }
        }
      }
}

void ref_column(int note_id) {
        // Update button in each octave/adc-channel
        for (int n = 0; n < 4; n++) {
          update_button(&buttons[note_id + n * 17]);
        }

        // calculate cross talk correction factors
        /* correction factor for 1k adc pull up resistors:
           1: 600/570 = 1.05
           2: 600/320 = 1.9
           3: 600/230 = 2.6
           4: 600/180 = 3.3
        */
        float oct_fact[4] = {1.0f};
        for (int n = 0; n < 4; n++) {
          float fact = 1.0f + buttons[note_id + n * 17].p * (0.05f / 0.9f / (MULTISAMPLE*4095.0f));
          oct_fact[n] = max(oct_fact[n], fact);
          for (int k = n+1; k < 4; k++) {
            fact = 1.0f + (min(buttons[note_id + n * 17].p, buttons[note_id + k * 17].p) - (KEY_DETECT+KEY_DETECT2))
                   * (0.9f / 0.95f / (MULTISAMPLE*4095.0f));
            oct_fact[n] = max(oct_fact[n], fact);
            oct_fact[k] = max(oct_fact[k], fact);
          }
        }
        bool set_oct[4];
        for (int b = 0; b < 17; b++) {
          if (b != note_id) {
            for (int n = 0; n < 4; n++) {
              set_oct[n] = true;
            }
            for (int n = 0; n < 4; n++) {
              for (int k = n+1; k < 4; k++) {
                if (buttons[note_id + n * 17].status != OFF && buttons[note_id + k * 17].status != OFF &&
                    (buttons[b + k * 17].status != OFF || buttons[b + n * 17].status != OFF)) {
                  if (buttons[b + n * 17].status != OFF && buttons[b + k * 17].status != OFF) {
                    // four corners pressed, do not set correction and reduce sensitivity
                    set_oct[n] = false;
                    set_oct[k] = false;
                    buttons[b + n * 17].key_detect3 = KEY_DETECT3 | 1;
                    buttons[b + k * 17].key_detect3 = KEY_DETECT3 | 1;
                  } else {
                    // three corners pressed, only reduce sensitivity for not pressed corner.
                    // | 1 as signal to the four corner correction algoritm
                    buttons[b + n * 17].key_detect3 = KEY_DETECT3 * (buttons[b + k * 17].status != OFF) | 1;
                    buttons[b + k * 17].key_detect3 = KEY_DETECT3 * (buttons[b + n * 17].status != OFF) | 1;
                  }
                }
              }
              // set correction if larger than current correction
              if (set_oct[n] && oct_fact[n] > buttons[b + n * 17].fact) {
                buttons[b + n * 17].fact = oct_fact[n];
              }
            }
          }
        }
}
