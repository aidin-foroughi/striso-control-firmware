/* The hooks and entry points of tests/ref/scan_seq_ref.c (the old key scan sequence). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "key_scanner.h"
#include "hal.h"
#ifdef __cplusplus
extern "C" {
#endif
#define ID_CONTROL 2
#define IDC_PORTAMENTO 0
#define IDC_OCT_UP 1
#define IDC_OCT_DOWN 2
#define IDC_ALT 3
extern volatile scan_stats_t ref_scan_stats;
void ref_ks_lost(void);
void ref_ks_boot_column(int col);
void ref_ks_column(int col);
bool ref_event_send1(int src, int id, int a);
void ref_adc_callback(const adcsample_t s[4]);
void ref_isr_first_scan(void);
void ref_set_waiting(void);
bool ref_waiting(void);
void ref_thread_column(int note_id);
#ifdef __cplusplus
}
#endif
