/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
/*
 * The message path, C side: events from the producers (the key scanner, the motion sensor, the aux
 * jack, the console, MIDI in) to the control thread, which hands each to synth_message and writes
 * it to the binary stream (control_thread.cpp). C++ code uses messaging.hpp; the queue is
 * inbox.hpp, the events and their frame event_queue.hpp and wire.hpp.
 */
#ifndef _MESSAGING_H_
#define _MESSAGING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Queue an event for the control thread: src (striso.h ID_*), id and n values (0..7), each
 * saturated to 16 bits. Never blocks; false if the sending thread's lane was full (counted in
 * messaging_drops) or n is out of range. From threads only, not interrupts. */
bool event_send(int src, int id, int n, const int* v);
/* the same with one value, and with two */
bool event_send1(int src, int id, int a);
bool event_send2(int src, int id, int a, int b);

/* the events refused since boot (StcE msg_underruns) */
uint32_t messaging_drops(void);

/* start the control thread ("send messages") */
void control_thread_start(void);

#ifdef __cplusplus
}
#endif

#endif
