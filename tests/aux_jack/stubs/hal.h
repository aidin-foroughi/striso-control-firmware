#pragma once
#include "ch.h"
#ifdef __cplusplus
extern "C" { /* as the real headers */
#endif
typedef uint16_t adcsample_t;
typedef int ioline_t;
typedef int iomode_t;
typedef struct { uint32_t difsel, calibration; } ADCConfig;
typedef struct { bool circular; int num_channels; void *end_cb, *error_cb; uint32_t cfgr, cfgr2, ccr, pcsel,
  ltr1, htr1, ltr2, htr2, ltr3, htr3; uint32_t smpr[2]; uint32_t sqr[4]; } ADCConversionGroup;
typedef struct { int dummy; } ADCDriver;
extern ADCDriver ADCD3;
#define ADC_CFGR_RES_14BITS 0
#define ADC_CFGR2_ROVSE 0
#define ADC_CFGR2_OVSR_N(n) 0
#define ADC_CFGR2_OVSS_N(n) 0
#define ADC_SELMASK_IN10 0
#define ADC_SELMASK_IN11 0
#define ADC_SMPR2_SMP_AN10(x) 0
#define ADC_SMPR2_SMP_AN11(x) 0
#define ADC_SMPR_SMP_810P5 0
#define ADC_SMPR_SMP_64P5 0
#define ADC_SQR1_SQ1_N(x) 0
#define ADC_SQR1_SQ2_N(x) 0
#define ADC_CHANNEL_IN10 10
#define ADC_CHANNEL_IN11 11
void adcStart(ADCDriver *d, const ADCConfig *c);
void adcConvert(ADCDriver *d, const ADCConversionGroup *g, adcsample_t *s, int depth);
#define cacheBufferInvalidate(p, n) ((void)(p), (void)(n))
enum { PAL_MODE_INPUT, PAL_MODE_INPUT_PULLUP, PAL_MODE_INPUT_PULLDOWN, PAL_MODE_INPUT_ANALOG,
       PAL_MODE_OUTPUT_PUSHPULL, PAL_MODE_OUTPUT_OPENDRAIN };
enum { LINE_AUX_ADC_T, LINE_AUX_ADC_R, LINE_AUX_UART2_RX, LINE_AUX_UART2_TX, LINE_AUX_JACK_DETECT, LINE_AUX_VDD };
void palSetLineMode(ioline_t l, int mode);
void palSetLine(ioline_t l);
void palClearLine(ioline_t l);
int palReadLine(ioline_t l);
#ifdef __cplusplus
}
#endif
