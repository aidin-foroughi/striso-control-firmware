// src/app/console.cpp (the USB console, its protocol and USB MIDI in) as the board builds it, with
// the device id and calibration (StcV) from the virtual board's flash.
#include "ch.h"
#include "hal.h"
extern "C" {
#include "sv_flash.h"
}

#include "console.cpp"
