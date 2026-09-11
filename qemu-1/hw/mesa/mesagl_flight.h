/*
 * QEMU MESA GL Pass-Through -- flight recorder
 *
 *  Copyright (c) 2026
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this library;
 * if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef MESAGL_FLIGHT_H
#define MESAGL_FLIGHT_H

#include "qemu/timer.h"

/* What happens at the register page, recorded the way Handmade Hero records its debug events:
 * a timestamp straight from rdtsc and three numbers, stored into a block that was allocated and
 * touched before the guest started. Nothing is formatted and nothing reaches a file while the
 * guest runs -- a text log costs more than the game can afford. vm/flight.py in the project
 * repository reads the result afterwards.
 *
 * Switched on with QEMU_3DFX_FLIGHT=<file>. Off, every hook is one predictable compare.
 * Every caller runs on a vCPU thread under the BQL, so there is no lock.
 */

typedef enum {
    /* id: register offset, value: the value written */
    FLIGHT_TRAP_WRITE = 1,
    /* id: register offset, value: FIFO entries that ran during the write */
    FLIGHT_TRAP_WRITE_DONE = 2,
    /* id: register offset, value: the value returned */
    FLIGHT_TRAP_READ = 3,
    /* id: 0, value: width times height */
    FLIGHT_READ_PIXELS = 4,
    /* id: FEnum, value: argument words -- level 2 only */
    FLIGHT_FIFO_CALL = 5,
    /* id: 0, value: FIFO words consumed -- level 2 only. Ends the last FIFO_CALL's time, which
     * would otherwise run on into the register's own work, a SwapBuffers for instance. */
    FLIGHT_FIFO_DONE = 6,
} FlightEventKind;

typedef enum {
    FLIGHT_LEVEL_OFF = 0,
    FLIGHT_LEVEL_TRAPS = 1,
    FLIGHT_LEVEL_FIFO_CALLS = 2,
} FlightLevel;

typedef enum {
    FLIGHT_FLUSH_START = 0,
    FLIGHT_FLUSH_LIBRARY_DETACH = 1,
    FLIGHT_FLUSH_EXIT = 2,
} FlightFlushReason;

/* Sixteen bytes, no padding. vm/flight.py reads exactly this layout. */
typedef struct {
    uint64_t ticks;
    uint16_t kind;
    uint16_t id;
    uint32_t value;
} FlightEvent;

extern int flight_level;
extern FlightEvent *flight_events;
extern uint64_t flight_event_mask;
extern uint64_t flight_events_recorded;
extern uint64_t *flight_fifo_call_counts;
extern uint32_t flight_fifo_call_count_size;

static inline void flight_record(const uint16_t kind, const uint16_t id, const uint32_t value)
{
    const uint64_t slot = flight_events_recorded & flight_event_mask;
    FlightEvent *event = &flight_events[slot];

    event->ticks = cpu_get_host_ticks();
    event->kind = kind;
    event->id = id;
    event->value = value;
    flight_events_recorded++;
}

#define FLIGHT_RECORD(minimum_level, kind, id, value) \
    do { if (unlikely(flight_level >= (minimum_level))) flight_record((kind), (id), (value)); } while (0)

/* The FEnum comes out of the guest's FIFO and is checked before it indexes host memory. */
#define FLIGHT_COUNT_FIFO_CALL(fenum) \
    do { if (unlikely(flight_level >= FLIGHT_LEVEL_TRAPS) && ((uint32_t)(fenum) < flight_fifo_call_count_size)) flight_fifo_call_counts[(fenum)]++; } while (0)

void flight_init(void);
void flight_flush(const FlightFlushReason reason);

#endif /* MESAGL_FLIGHT_H */
