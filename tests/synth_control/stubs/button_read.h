/* Host stand-in for src/drivers/button_read.h (tests/synth_control): the keys' factors (StcY) */
#ifndef SC_TEST_STUB_BUTTON_READ_H
#define SC_TEST_STUB_BUTTON_READ_H
#ifdef __cplusplus
extern "C" {
#endif
int buttonKeyCount(void);
float buttonGetKeyFactor(int n);
#ifdef __cplusplus
}
#endif
#endif
