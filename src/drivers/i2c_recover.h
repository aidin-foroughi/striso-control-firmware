/*
 * I2C transfers that can't hang, and bus recovery (motion sensor, codec).
 *
 * A transfer gives up after I2C_TRANSFER_TIMEOUT instead of waiting forever (a slave holding SCL
 * blocked the calling thread for good). After a failed transfer the driver is stopped (ChibiOS:
 * a timed-out driver is locked until restarted), a slave holding SDA low is clocked free (up to
 * 9 SCL pulses, then a STOP), the pins go back to the I2C function and the driver is restarted.
 */
#ifndef I2C_RECOVER_H
#define I2C_RECOVER_H

#include "hal.h"

#define I2C_TRANSFER_TIMEOUT TIME_MS2I(10) /* a 13-byte transfer takes 1.3 ms at 100 kHz */

/*
 * The bus clear: with SCL high and SDA low (a slave interrupted in the middle of a byte), clock
 * SCL until the slave releases SDA, up to 9 times, then send a STOP condition. The pins are left
 * as open-drain outputs, released (high).
 */
static inline void i2c_bus_clear(ioline_t scl, ioline_t sda) {
  palSetLine(scl);
  palSetLineMode(scl, PAL_MODE_OUTPUT_OPENDRAIN);
  palSetLineMode(sda, PAL_MODE_INPUT);
  for (int i = 0; i < 9 && !palReadLine(sda); i++) {
    palClearLine(scl);
    chThdSleepMilliseconds(1);
    palSetLine(scl);
    chThdSleepMilliseconds(1);
  }
  // STOP: SDA low to high while SCL is high
  palClearLine(scl);
  palClearLine(sda);
  palSetLineMode(sda, PAL_MODE_OUTPUT_OPENDRAIN);
  chThdSleepMilliseconds(1);
  palSetLine(scl);
  chThdSleepMilliseconds(1);
  palSetLine(sda);
  chThdSleepMilliseconds(1);
}

/*
 * After a failed transfer: restart the driver with a bus clear in between. pin_mode: the pins'
 * I2C mode (alternate function, open drain, speed).
 */
static inline void i2c_recover(I2CDriver *i2cp, const I2CConfig *cfg, ioline_t scl, ioline_t sda,
                               iomode_t pin_mode) {
  i2cStop(i2cp);
  i2c_bus_clear(scl, sda);
  palSetLineMode(scl, pin_mode);
  palSetLineMode(sda, pin_mode);
  i2cStart(i2cp, cfg);
}

#endif
