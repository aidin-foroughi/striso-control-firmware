/**
 * Copyright (C) 2013, 2014, 2015 Johannes Taelman
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

// The USB console and USB MIDI in (console.cpp), for the main thread
#ifndef __PCONNECTION_H
#define __PCONNECTION_H

#ifdef __cplusplus
extern "C" {
#endif

// the USB drivers, then the device on the bus (waits 500 ms first)
void InitPConnection(void);
// the console's bytes and the USB MIDI events that arrived; never blocks for long (a reply's
// wait for the host times out)
void PExReceive(void);
// one console byte (PExReceive's; the tests feed bytes directly)
void PExReceiveByte(unsigned char c);

#ifdef __cplusplus
}
#endif

#endif
