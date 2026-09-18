/*
 * sinetest_ds -- the same tones as sinetest, but through DirectSound
 *
 * sinetest plays through waveOut: hand over a finished buffer, get told when it
 * is done. A game instead streams into a looping ring buffer while the hardware
 * reads from it, polling the play cursor to decide how far ahead it may write.
 * That is what Quake III does, and Quake III is the only thing here that
 * crackles -- so this program isolates the one variable that differs.
 *
 * Same six formats, same six pitches, so vm/sinecheck.py reads both alike.
 *
 * With the argument "cursor" it plays one tone instead and measures the only thing a
 * streaming program really depends on: the play position the hardware reports. On real
 * hardware that position runs; in a device model that moves it once per audio period it is
 * a staircase, and a program that computes its write position from it writes sometimes too
 * early and sometimes too late. Every query goes into an array in memory and is counted up
 * afterwards -- writing a line per query would change the very timing under test.
 *
 * Written for Windows 98 and the MinGW cross toolchain, C89 throughout.
 */

#include <windows.h>
#include <dsound.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define REPORT_FILE_NAME        "C:\\DSOUT.TXT"
#define CURSOR_REPORT_FILE_NAME "C:\\DSCURSOR.TXT"

/* The cursor measurement: one tone, queried as fast as the loop goes round. */
#define CURSOR_SECONDS          5.0
#define CURSOR_SAMPLE_CAPACITY  400000
#define CURSOR_TEST_CASE        3       /* 22050 Hz, 16 bit, stereo */
#define CURSOR_STEPS_PRINTED    12

#define TONE_SECONDS            3.0
#define SILENCE_SECONDS         0.4
#define RING_BUFFER_SECONDS     1.0
/* Quake III's s_mixahead defaults to 0.2 s -- the same write-ahead is used here. */
#define WRITE_AHEAD_SECONDS     0.2
#define POLL_SLEEP_MILLISECONDS 5

#define AMPLITUDE_FRACTION      0.6
#define EIGHT_BIT_CENTRE        128
#define EIGHT_BIT_PEAK          127
#define SIXTEEN_BIT_PEAK        32767

struct test_case {
    unsigned int samples_per_second;
    unsigned int bits_per_sample;
    unsigned int channel_count;
    unsigned int tone_hertz;
};

static const struct test_case test_cases[] = {
    { 11025,  8, 1,  400 },
    { 11025, 16, 2,  600 },
    { 22050,  8, 1,  800 },
    { 22050, 16, 2, 1000 },
    { 44100,  8, 1, 1200 },
    { 44100, 16, 2, 1400 }
};

#define TEST_CASE_COUNT (sizeof(test_cases) / sizeof(test_cases[0]))

typedef HRESULT (WINAPI *DirectSoundCreateFunction)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);

static FILE *report_file = NULL;

static void report(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    fflush(stdout);

    if (report_file != NULL) {
        va_start(arguments, format);
        vfprintf(report_file, format, arguments);
        va_end(arguments);
        fflush(report_file);
    }
}

static void fill_wave_format(WAVEFORMATEX *format, const struct test_case *test)
{
    memset(format, 0, sizeof(*format));
    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = (WORD)test->channel_count;
    format->nSamplesPerSec = test->samples_per_second;
    format->wBitsPerSample = (WORD)test->bits_per_sample;
    format->nBlockAlign = (WORD)((test->bits_per_sample / 8) * test->channel_count);
    format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
    format->cbSize = 0;
}

/*
 * Writes count frames of the tone into the buffer, continuing the phase from
 * frame_index so that successive writes join without a step.
 */
static void write_tone_frames(unsigned char *destination, unsigned long frame_count,
                              unsigned long frame_index, const struct test_case *test)
{
    const double two_pi = 6.283185307179586;
    double radians_per_frame = two_pi * test->tone_hertz / (double)test->samples_per_second;
    unsigned long offset = 0;
    unsigned long i;

    for (i = 0; i < frame_count; ++i) {
        double wave_value = sin(radians_per_frame * (double)(frame_index + i)) * AMPLITUDE_FRACTION;
        unsigned int channel_index;

        for (channel_index = 0; channel_index < test->channel_count; ++channel_index) {
            if (test->bits_per_sample == 8) {
                int sample = EIGHT_BIT_CENTRE + (int)(wave_value * EIGHT_BIT_PEAK);

                destination[offset] = (unsigned char)sample;
                offset += 1;
            } else {
                int sample = (int)(wave_value * SIXTEEN_BIT_PEAK);

                destination[offset] = (unsigned char)(sample & 0xff);
                destination[offset + 1] = (unsigned char)((sample >> 8) & 0xff);
                offset += 2;
            }
        }
    }
}

/* Writes into the ring buffer at byte_offset, splitting at the wrap. */
static int write_into_ring(LPDIRECTSOUNDBUFFER buffer, unsigned long byte_offset,
                           unsigned long byte_count, unsigned long frame_index,
                           unsigned int bytes_per_frame, const struct test_case *test)
{
    void *first_part = NULL;
    void *second_part = NULL;
    DWORD first_bytes = 0;
    DWORD second_bytes = 0;
    HRESULT result;

    result = IDirectSoundBuffer_Lock(buffer, byte_offset, byte_count,
                                     &first_part, &first_bytes,
                                     &second_part, &second_bytes, 0);
    if (result != DS_OK) {
        return 0;
    }

    write_tone_frames((unsigned char *)first_part, first_bytes / bytes_per_frame,
                      frame_index, test);
    if (second_bytes > 0) {
        write_tone_frames((unsigned char *)second_part, second_bytes / bytes_per_frame,
                          frame_index + first_bytes / bytes_per_frame, test);
    }

    IDirectSoundBuffer_Unlock(buffer, first_part, first_bytes, second_part, second_bytes);
    return 1;
}

static int play_one_case(LPDIRECTSOUND direct_sound, const struct test_case *test)
{
    WAVEFORMATEX wave_format;
    DSBUFFERDESC buffer_description;
    LPDIRECTSOUNDBUFFER ring_buffer = NULL;
    HRESULT result;
    unsigned int bytes_per_frame = (test->bits_per_sample / 8) * test->channel_count;
    unsigned long ring_bytes;
    unsigned long write_ahead_bytes;
    unsigned long written_frames = 0;
    unsigned long write_cursor_bytes = 0;
    unsigned int poll_count = 0;
    unsigned int stalled_polls = 0;
    DWORD previous_play_cursor = 0xffffffff;
    DWORD started_at;

    report("%5u Hz  %2u bit  %s  tone %4u Hz  ",
           test->samples_per_second, test->bits_per_sample,
           test->channel_count == 1 ? "mono  " : "stereo",
           test->tone_hertz);

    fill_wave_format(&wave_format, test);
    ring_bytes = (unsigned long)(RING_BUFFER_SECONDS * test->samples_per_second) * bytes_per_frame;
    write_ahead_bytes = (unsigned long)(WRITE_AHEAD_SECONDS * test->samples_per_second) * bytes_per_frame;

    memset(&buffer_description, 0, sizeof(buffer_description));
    buffer_description.dwSize = sizeof(buffer_description);
    buffer_description.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    buffer_description.dwBufferBytes = ring_bytes;
    buffer_description.lpwfxFormat = &wave_format;

    result = IDirectSound_CreateSoundBuffer(direct_sound, &buffer_description, &ring_buffer, NULL);
    if (result != DS_OK) {
        report("-> CreateSoundBuffer failed (0x%08lx)\n", (unsigned long)result);
        return 0;
    }

    /* Prime the whole ring, then let it loop while we keep writing ahead. */
    write_into_ring(ring_buffer, 0, ring_bytes, 0, bytes_per_frame, test);
    written_frames = ring_bytes / bytes_per_frame;
    write_cursor_bytes = 0;

    IDirectSoundBuffer_SetCurrentPosition(ring_buffer, 0);
    result = IDirectSoundBuffer_Play(ring_buffer, 0, 0, DSBPLAY_LOOPING);
    if (result != DS_OK) {
        report("-> Play failed (0x%08lx)\n", (unsigned long)result);
        IDirectSoundBuffer_Release(ring_buffer);
        return 0;
    }

    started_at = GetTickCount();
    while (GetTickCount() - started_at < (DWORD)(TONE_SECONDS * 1000)) {
        DWORD play_cursor = 0;
        DWORD safe_write_cursor = 0;
        unsigned long target_bytes;
        unsigned long pending_bytes;

        Sleep(POLL_SLEEP_MILLISECONDS);
        if (IDirectSoundBuffer_GetCurrentPosition(ring_buffer, &play_cursor, &safe_write_cursor) != DS_OK) {
            continue;
        }
        poll_count++;
        if (play_cursor == previous_play_cursor) {
            stalled_polls++;
        }
        previous_play_cursor = play_cursor;

        target_bytes = (play_cursor + write_ahead_bytes) % ring_bytes;
        if (target_bytes >= write_cursor_bytes) {
            pending_bytes = target_bytes - write_cursor_bytes;
        } else {
            pending_bytes = ring_bytes - write_cursor_bytes + target_bytes;
        }
        pending_bytes -= pending_bytes % bytes_per_frame;
        if (pending_bytes == 0) {
            continue;
        }

        write_into_ring(ring_buffer, write_cursor_bytes, pending_bytes,
                        written_frames, bytes_per_frame, test);
        written_frames += pending_bytes / bytes_per_frame;
        write_cursor_bytes = (write_cursor_bytes + pending_bytes) % ring_bytes;
    }

    IDirectSoundBuffer_Stop(ring_buffer);
    IDirectSoundBuffer_Release(ring_buffer);

    report("-> played, %u queries, %u of them without progress (%u %%)\n",
           poll_count, stalled_polls,
           poll_count ? (stalled_polls * 100 / poll_count) : 0);
    Sleep((DWORD)(SILENCE_SECONDS * 1000));
    return 1;
}

/* One query of the play position: when, and what came back. */
struct cursor_sample {
    LONGLONG ticks;
    DWORD play_cursor;
    DWORD write_cursor;
};

static struct cursor_sample cursor_samples[CURSOR_SAMPLE_CAPACITY];

/* How often each step size turned up, counted into a small table instead of sorted. */
struct step_count {
    unsigned long step_frames;
    unsigned long count;
};

static void count_step(struct step_count *table, unsigned int table_size,
                       unsigned int *used, unsigned long step_frames)
{
    unsigned int index;

    for (index = 0; index < *used; ++index) {
        if (table[index].step_frames == step_frames) {
            table[index].count++;
            return;
        }
    }
    if (*used < table_size) {
        table[*used].step_frames = step_frames;
        table[*used].count = 1;
        (*used)++;
    }
}

static int measure_cursor(LPDIRECTSOUND direct_sound)
{
    const struct test_case *test = &test_cases[CURSOR_TEST_CASE];
    WAVEFORMATEX wave_format;
    DSBUFFERDESC buffer_description;
    LPDIRECTSOUNDBUFFER ring_buffer = NULL;
    HRESULT result;
    unsigned int bytes_per_frame = (test->bits_per_sample / 8) * test->channel_count;
    unsigned long ring_bytes;
    unsigned long write_ahead_bytes;
    unsigned long written_frames;
    unsigned long write_cursor_bytes = 0;
    unsigned long sample_count = 0;
    unsigned long sample_index;
    unsigned long standing_queries = 0;
    unsigned long total_step_frames = 0;
    LARGE_INTEGER frequency;
    LARGE_INTEGER now;
    LARGE_INTEGER started_at;
    double measured_seconds;
    struct step_count steps[64];
    unsigned int steps_used = 0;
    unsigned int step_index;

    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
        report("QueryPerformanceFrequency is not available.\n");
        return 0;
    }

    fill_wave_format(&wave_format, test);
    ring_bytes = (unsigned long)(RING_BUFFER_SECONDS * test->samples_per_second) * bytes_per_frame;
    write_ahead_bytes = (unsigned long)(WRITE_AHEAD_SECONDS * test->samples_per_second) * bytes_per_frame;

    memset(&buffer_description, 0, sizeof(buffer_description));
    buffer_description.dwSize = sizeof(buffer_description);
    buffer_description.dwFlags = DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS;
    buffer_description.dwBufferBytes = ring_bytes;
    buffer_description.lpwfxFormat = &wave_format;

    result = IDirectSound_CreateSoundBuffer(direct_sound, &buffer_description, &ring_buffer, NULL);
    if (result != DS_OK) {
        report("CreateSoundBuffer failed (0x%08lx)\n", (unsigned long)result);
        return 0;
    }

    write_into_ring(ring_buffer, 0, ring_bytes, 0, bytes_per_frame, test);
    written_frames = ring_bytes / bytes_per_frame;

    IDirectSoundBuffer_SetCurrentPosition(ring_buffer, 0);
    result = IDirectSoundBuffer_Play(ring_buffer, 0, 0, DSBPLAY_LOOPING);
    if (result != DS_OK) {
        report("Play failed (0x%08lx)\n", (unsigned long)result);
        IDirectSoundBuffer_Release(ring_buffer);
        return 0;
    }

    report("%5u Hz  %2u bit  %s  tone %4u Hz, %.1f s, queried without pause\n",
           test->samples_per_second, test->bits_per_sample,
           test->channel_count == 1 ? "mono  " : "stereo",
           test->tone_hertz, CURSOR_SECONDS);

    QueryPerformanceCounter(&started_at);
    for (;;) {
        DWORD play_cursor = 0;
        DWORD safe_write_cursor = 0;
        unsigned long target_bytes;
        unsigned long pending_bytes;

        QueryPerformanceCounter(&now);
        measured_seconds = (double)(now.QuadPart - started_at.QuadPart) / (double)frequency.QuadPart;
        if (measured_seconds >= CURSOR_SECONDS || sample_count >= CURSOR_SAMPLE_CAPACITY) {
            break;
        }
        if (IDirectSoundBuffer_GetCurrentPosition(ring_buffer, &play_cursor, &safe_write_cursor) != DS_OK) {
            continue;
        }

        cursor_samples[sample_count].ticks = now.QuadPart;
        cursor_samples[sample_count].play_cursor = play_cursor;
        cursor_samples[sample_count].write_cursor = safe_write_cursor;
        sample_count++;

        /* Keep writing ahead the way a game does, so the measurement sits in the real case. */
        target_bytes = (play_cursor + write_ahead_bytes) % ring_bytes;
        if (target_bytes >= write_cursor_bytes) {
            pending_bytes = target_bytes - write_cursor_bytes;
        } else {
            pending_bytes = ring_bytes - write_cursor_bytes + target_bytes;
        }
        pending_bytes -= pending_bytes % bytes_per_frame;
        if (pending_bytes >= write_ahead_bytes / 2) {
            write_into_ring(ring_buffer, write_cursor_bytes, pending_bytes,
                            written_frames, bytes_per_frame, test);
            written_frames += pending_bytes / bytes_per_frame;
            write_cursor_bytes = (write_cursor_bytes + pending_bytes) % ring_bytes;
        }
    }

    IDirectSoundBuffer_Stop(ring_buffer);
    IDirectSoundBuffer_Release(ring_buffer);

    for (sample_index = 1; sample_index < sample_count; ++sample_index) {
        DWORD previous_cursor = cursor_samples[sample_index - 1].play_cursor;
        DWORD current_cursor = cursor_samples[sample_index].play_cursor;
        unsigned long step_bytes;
        unsigned long step_frames;

        if (current_cursor >= previous_cursor) {
            step_bytes = current_cursor - previous_cursor;
        } else {
            step_bytes = ring_bytes - previous_cursor + current_cursor;
        }
        step_frames = step_bytes / bytes_per_frame;
        if (step_frames == 0) {
            standing_queries++;
        }
        total_step_frames += step_frames;
        count_step(steps, (unsigned int)(sizeof(steps) / sizeof(steps[0])), &steps_used, step_frames);
    }

    report("\n");
    report("queries            %lu in %.2f s = %.0f per second\n",
           sample_count, measured_seconds,
           measured_seconds > 0.0 ? (double)sample_count / measured_seconds : 0.0);
    report("without progress   %lu = %lu %%\n",
           standing_queries,
           sample_count > 1 ? standing_queries * 100 / (sample_count - 1) : 0);
    report("average step       %.1f frames = %.3f ms\n",
           sample_count > 1 ? (double)total_step_frames / (double)(sample_count - 1) : 0.0,
           sample_count > 1
               ? (double)total_step_frames / (double)(sample_count - 1)
                 * 1000.0 / (double)test->samples_per_second
               : 0.0);
    report("\n");
    report("the steps the position moved in:\n");

    for (step_index = 0; step_index < CURSOR_STEPS_PRINTED; ++step_index) {
        unsigned int scan;
        unsigned int largest = 0;
        unsigned long largest_count = 0;
        int found = 0;

        for (scan = 0; scan < steps_used; ++scan) {
            if (steps[scan].count > largest_count) {
                largest_count = steps[scan].count;
                largest = scan;
                found = 1;
            }
        }
        if (!found) {
            break;
        }
        report("   %+8lu frames  %8lu times  = %8.3f ms\n",
               steps[largest].step_frames, steps[largest].count,
               (double)steps[largest].step_frames * 1000.0 / (double)test->samples_per_second);
        steps[largest].count = 0;
    }

    report("\n");
    report("A position that runs shows many different small steps. A staircase shows a zero\n");
    report("and one step the size of an audio period -- see docs/LOG.md [316].\n");
    return 1;
}

int main(int argument_count, char **arguments)
{
    HMODULE dsound_module;
    DirectSoundCreateFunction create_function;
    LPDIRECTSOUND direct_sound = NULL;
    LPDIRECTSOUNDBUFFER primary_buffer = NULL;
    DSBUFFERDESC primary_description;
    WAVEFORMATEX primary_format;
    HRESULT result;
    unsigned int case_index;
    unsigned int played_count = 0;

    int measure_cursor_only = (argument_count > 1 && strcmp(arguments[1], "cursor") == 0);

    report_file = fopen(measure_cursor_only ? CURSOR_REPORT_FILE_NAME : REPORT_FILE_NAME, "w");
    if (measure_cursor_only) {
        report("sinetest_ds cursor -- how the play position moves that a streaming program reads\n");
    } else {
        report("sinetest_ds -- the same tones as sinetest, but through DirectSound\n");
    }
    report("-------------------------------------------------------------------\n");

    dsound_module = LoadLibrary("dsound.dll");
    if (dsound_module == NULL) {
        report("dsound.dll cannot be loaded.\n");
        return 1;
    }
    create_function = (DirectSoundCreateFunction)GetProcAddress(dsound_module, "DirectSoundCreate");
    if (create_function == NULL) {
        report("DirectSoundCreate not found.\n");
        return 1;
    }

    result = create_function(NULL, &direct_sound, NULL);
    if (result != DS_OK) {
        report("DirectSoundCreate failed (0x%08lx)\n", (unsigned long)result);
        return 1;
    }

    /*
     * DSSCL_PRIORITY is what lets an application set the primary buffer format.
     * Whether that succeeds is worth reporting: Quake III ends up playing 8 bit
     * here, which is the DirectSound default when the call does not take.
     */
    result = IDirectSound_SetCooperativeLevel(direct_sound, GetDesktopWindow(), DSSCL_PRIORITY);
    report("SetCooperativeLevel(PRIORITY): %s (0x%08lx)\n",
           result == DS_OK ? "ok" : "failed", (unsigned long)result);

    memset(&primary_description, 0, sizeof(primary_description));
    primary_description.dwSize = sizeof(primary_description);
    primary_description.dwFlags = DSBCAPS_PRIMARYBUFFER;
    result = IDirectSound_CreateSoundBuffer(direct_sound, &primary_description, &primary_buffer, NULL);
    if (result == DS_OK) {
        fill_wave_format(&primary_format, &test_cases[3]);   /* 22050, 16 Bit, stereo */
        result = IDirectSoundBuffer_SetFormat(primary_buffer, &primary_format);
        report("Setting the primary buffer to 22050/16/stereo: %s (0x%08lx)\n",
               result == DS_OK ? "ok" : "failed", (unsigned long)result);
    } else {
        report("Cannot get the primary buffer (0x%08lx)\n", (unsigned long)result);
    }
    report("\n");

    if (measure_cursor_only) {
        played_count = measure_cursor(direct_sound);
        report("Report: %s\n", CURSOR_REPORT_FILE_NAME);
    } else {
        for (case_index = 0; case_index < TEST_CASE_COUNT; ++case_index) {
            played_count += play_one_case(direct_sound, &test_cases[case_index]);
        }

        report("\n%u of %u test cases played.\n", played_count, (unsigned int)TEST_CASE_COUNT);
        report("Report: %s\n", REPORT_FILE_NAME);
    }

    if (primary_buffer != NULL) {
        IDirectSoundBuffer_Release(primary_buffer);
    }
    IDirectSound_Release(direct_sound);
    if (report_file != NULL) {
        fclose(report_file);
    }
    if (measure_cursor_only) {
        return played_count == 1 ? 0 : 1;
    }
    return played_count == TEST_CASE_COUNT ? 0 : 1;
}
