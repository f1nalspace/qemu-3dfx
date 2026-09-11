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

#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"

#include "mesagl_impl.h"
#include "mesagl_flight.h"

#define FLIGHT_ENVIRONMENT_FILE  "QEMU_3DFX_FLIGHT"
#define FLIGHT_ENVIRONMENT_LEVEL "QEMU_3DFX_FLIGHT_LEVEL"
#define FLIGHT_ENVIRONMENT_MB    "QEMU_3DFX_FLIGHT_MB"

#define FLIGHT_DEFAULT_MEGABYTES 1024
#define FLIGHT_MINIMUM_MEGABYTES 1
#define FLIGHT_BYTES_PER_MEGABYTE (1024ULL * 1024ULL)
#define FLIGHT_DECIMAL_BASE 10

/* The file is a row of chunks, each opening with this header. */
#define FLIGHT_FILE_MAGIC "Q3DFLGT1"
#define FLIGHT_FILE_MAGIC_BYTES 8

typedef enum {
    FLIGHT_CHUNK_NAMES = 1,
    FLIGHT_CHUNK_EVENTS = 2,
    FLIGHT_CHUNK_FIFO_CALL_COUNTS = 3,
} FlightChunkType;

/* Sixty-four bytes, no padding. vm/flight.py reads exactly this layout. */
typedef struct {
    char magic[FLIGHT_FILE_MAGIC_BYTES];
    uint32_t chunk_type;
    uint32_t record_bytes;
    uint64_t payload_bytes;
    uint64_t first_event_index;
    uint64_t events_lost;
    int64_t ticks_at_write;
    int64_t realtime_ns_at_write;
    uint32_t level;
    uint32_t reason;
} FlightChunkHeader;

int flight_level;
FlightEvent *flight_events;
uint64_t flight_event_mask;
uint64_t flight_events_recorded;
uint64_t *flight_fifo_call_counts;
uint32_t flight_fifo_call_count_size;

static FILE *flight_file;
static uint64_t flight_events_written;
static int flight_configured_level;

static long flight_environment_number(const char *name, const long fallback)
{
    const char *setting = getenv(name);
    char *end = NULL;
    long number;

    if (!setting || !setting[0])
        return fallback;
    number = strtol(setting, &end, FLIGHT_DECIMAL_BASE);
    return (end && *end == '\0')? number:fallback;
}

/* rdtsc on both sides of the clock read, so the pair describes one instant as closely as the
 * clock allows. vm/flight.py turns ticks into seconds with two such pairs. */
static void flight_fill_header(FlightChunkHeader *header, const FlightChunkType chunk_type, const uint32_t record_bytes, const uint64_t payload_bytes, const FlightFlushReason reason)
{
    const int64_t ticks_before = cpu_get_host_ticks();
    const int64_t realtime_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    const int64_t ticks_after = cpu_get_host_ticks();

    memset(header, 0, sizeof(*header));
    memcpy(header->magic, FLIGHT_FILE_MAGIC, FLIGHT_FILE_MAGIC_BYTES);
    header->chunk_type = chunk_type;
    header->record_bytes = record_bytes;
    header->payload_bytes = payload_bytes;
    header->ticks_at_write = ticks_before + (ticks_after - ticks_before) / 2;
    header->realtime_ns_at_write = realtime_ns;
    header->level = flight_configured_level;
    header->reason = reason;
}

static void flight_write_names(void)
{
    FlightChunkHeader header;
    uint64_t payload_bytes = 0;
    uint32_t function_number;

    for (function_number = 0; function_number < flight_fifo_call_count_size; function_number++) {
        const char *symbol = GLFEnumSymbol(function_number);
        const size_t symbol_bytes = strlen(symbol) + 1;
        payload_bytes += symbol_bytes;
    }

    flight_fill_header(&header, FLIGHT_CHUNK_NAMES, 0, payload_bytes, FLIGHT_FLUSH_START);
    fwrite(&header, sizeof(header), 1, flight_file);
    for (function_number = 0; function_number < flight_fifo_call_count_size; function_number++) {
        const char *symbol = GLFEnumSymbol(function_number);
        const size_t symbol_bytes = strlen(symbol) + 1;
        fwrite(symbol, 1, symbol_bytes, flight_file);
    }
}

/* Appends every event recorded since the last flush, then the FIFO call counts as they stand.
 * Events the ring overwrote in between are counted in the header, not silently skipped. */
void flight_flush(const FlightFlushReason reason)
{
    FlightChunkHeader header;
    const uint64_t recorded = flight_events_recorded;
    const uint64_t capacity = flight_event_mask + 1;
    uint64_t pending = recorded - flight_events_written;
    uint64_t lost = 0;

    if (!flight_file)
        return;

    if (pending > capacity) {
        lost = pending - capacity;
        pending = capacity;
    }

    const uint64_t first_index = recorded - pending;
    const uint64_t first_slot = first_index & flight_event_mask;
    const uint64_t slots_until_end = capacity - first_slot;
    const uint64_t first_part = MIN(pending, slots_until_end);
    const uint64_t second_part = pending - first_part;
    const uint64_t event_payload_bytes = pending * sizeof(FlightEvent);
    const uint64_t count_payload_bytes = (uint64_t)flight_fifo_call_count_size * sizeof(uint64_t);

    flight_fill_header(&header, FLIGHT_CHUNK_EVENTS, sizeof(FlightEvent), event_payload_bytes, reason);
    header.first_event_index = first_index;
    header.events_lost = lost;
    fwrite(&header, sizeof(header), 1, flight_file);
    fwrite(&flight_events[first_slot], sizeof(FlightEvent), first_part, flight_file);
    fwrite(&flight_events[0], sizeof(FlightEvent), second_part, flight_file);

    flight_fill_header(&header, FLIGHT_CHUNK_FIFO_CALL_COUNTS, sizeof(uint64_t), count_payload_bytes, reason);
    fwrite(&header, sizeof(header), 1, flight_file);
    fwrite(flight_fifo_call_counts, sizeof(uint64_t), flight_fifo_call_count_size, flight_file);

    fflush(flight_file);
    flight_events_written = recorded;
}

static void flight_at_exit(void)
{
    if (!flight_file)
        return;
    flight_level = FLIGHT_LEVEL_OFF;
    flight_flush(FLIGHT_FLUSH_EXIT);
    fclose(flight_file);
    flight_file = NULL;
    fprintf(stderr, "qemu-3dfx flight: %" PRIu64 " events recorded, written at exit\n", flight_events_recorded);
}

void flight_init(void)
{
    const char *file_name = getenv(FLIGHT_ENVIRONMENT_FILE);

    if (!file_name || !file_name[0] || flight_file)
        return;

    const long requested_level = flight_environment_number(FLIGHT_ENVIRONMENT_LEVEL, FLIGHT_LEVEL_TRAPS);
    const long requested_megabytes = flight_environment_number(FLIGHT_ENVIRONMENT_MB, FLIGHT_DEFAULT_MEGABYTES);
    const int level = (requested_level >= FLIGHT_LEVEL_FIFO_CALLS)? FLIGHT_LEVEL_FIFO_CALLS:FLIGHT_LEVEL_TRAPS;
    const uint64_t megabytes = (requested_megabytes >= FLIGHT_MINIMUM_MEGABYTES)? (uint64_t)requested_megabytes:FLIGHT_MINIMUM_MEGABYTES;
    const uint64_t requested_bytes = megabytes * FLIGHT_BYTES_PER_MEGABYTE;
    const uint64_t requested_events = requested_bytes / sizeof(FlightEvent);
    const uint64_t event_capacity = pow2floor(requested_events);
    const uint64_t event_bytes = event_capacity * sizeof(FlightEvent);

    flight_events = g_try_malloc(event_bytes);
    if (!flight_events) {
        fprintf(stderr, "qemu-3dfx flight: %" PRIu64 " MB not available, recorder stays off\n", megabytes);
        return;
    }
    /* Every page is touched here, so that recording never waits for the host kernel to hand
     * one out in the middle of a frame. */
    memset(flight_events, 0, event_bytes);

    flight_fifo_call_count_size = FEnum_zzMGLFuncEnum_max;
    flight_fifo_call_counts = g_new0(uint64_t, flight_fifo_call_count_size);

    flight_file = fopen(file_name, "wb");
    if (!flight_file) {
        fprintf(stderr, "qemu-3dfx flight: cannot open %s, recorder stays off\n", file_name);
        g_free(flight_events);
        g_free(flight_fifo_call_counts);
        flight_events = NULL;
        flight_fifo_call_counts = NULL;
        return;
    }

    flight_event_mask = event_capacity - 1;
    flight_configured_level = level;
    flight_write_names();
    flight_flush(FLIGHT_FLUSH_START);
    atexit(flight_at_exit);

    fprintf(stderr, "qemu-3dfx flight: %s, level %d, %" PRIu64 " MB, room for %" PRIu64 " events\n",
            file_name, level, (uint64_t)(event_bytes / FLIGHT_BYTES_PER_MEGABYTE), event_capacity);
    /* Last, because this is what switches every hook on. */
    flight_level = level;
}
