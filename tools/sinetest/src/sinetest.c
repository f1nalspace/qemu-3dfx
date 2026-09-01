/*
 * sinetest -- waveOut test case for the QEMU sound device models
 *
 * Plays one sine tone per format, each at its own pitch and separated by
 * silence, so a recording taken on the host can be split up again and checked
 * tone by tone. A sample rate the device model gets wrong shifts the pitch by
 * exactly the ratio of the two rates, which is what makes this measurable
 * instead of a matter of opinion.
 *
 * Written for Windows 98 and the MinGW cross toolchain, C89 throughout.
 */

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define REPORT_FILE_NAME        "C:\\SINEOUT.TXT"

#define TONE_SECONDS            1.0
#define SILENCE_SECONDS         0.3
#define AMPLITUDE_FRACTION      0.6

/* Unsigned 8 bit PCM sits around 128, signed 16 bit around zero. */
#define EIGHT_BIT_CENTRE        128
#define EIGHT_BIT_PEAK          127
#define SIXTEEN_BIT_PEAK        32767

#define WAIT_POLL_MILLISECONDS  10
#define WAIT_LIMIT_SECONDS      10

struct test_case {
    unsigned int samples_per_second;
    unsigned int bits_per_sample;
    unsigned int channel_count;
    unsigned int tone_hertz;
};

/*
 * The four rates the ES1370 DAC1 can clock natively are 5512, 11025, 22050 and
 * 44100; DAC2 derives its rate from a divider and takes anything. Each case
 * gets a pitch of its own so the recording is self-labelling.
 */
static const struct test_case test_cases[] = {
    { 11025,  8, 1,  400 },
    { 11025, 16, 2,  600 },
    { 22050,  8, 1,  800 },
    { 22050, 16, 2, 1000 },
    { 44100,  8, 1, 1200 },
    { 44100, 16, 2, 1400 }
};

#define TEST_CASE_COUNT (sizeof(test_cases) / sizeof(test_cases[0]))

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

static const char *wave_error_text(MMRESULT result)
{
    switch (result) {
    case MMSYSERR_NOERROR:      return "kein Fehler";
    case MMSYSERR_ALLOCATED:    return "Geraet bereits belegt";
    case MMSYSERR_BADDEVICEID:  return "ungueltige Geraetenummer";
    case MMSYSERR_NODRIVER:     return "kein Treiber";
    case MMSYSERR_NOMEM:        return "kein Speicher";
    case MMSYSERR_INVALHANDLE:  return "ungueltige Kennung";
    case WAVERR_BADFORMAT:      return "Format nicht unterstuetzt";
    case WAVERR_STILLPLAYING:   return "spielt noch";
    case WAVERR_UNPREPARED:     return "Puffer nicht vorbereitet";
    default:                    return "unbekannt";
    }
}

static void list_devices(void)
{
    UINT device_count = waveOutGetNumDevs();
    UINT device_index;

    report("Wiedergabegeraete: %u\n", device_count);

    for (device_index = 0; device_index < device_count; ++device_index) {
        WAVEOUTCAPS capabilities;
        MMRESULT result = waveOutGetDevCaps(device_index, &capabilities, sizeof(capabilities));

        if (result != MMSYSERR_NOERROR) {
            report("  [%u] Abfrage fehlgeschlagen: %s\n", device_index, wave_error_text(result));
            continue;
        }
        report("  [%u] %s  Kanaele=%u  Formate=0x%08lx\n",
               device_index, capabilities.szPname,
               (unsigned int)capabilities.wChannels,
               (unsigned long)capabilities.dwFormats);
    }
}

/*
 * Fills the buffer with a sine of the requested pitch, followed by silence.
 * Returns the number of bytes written, zero if the buffer is too small.
 */
static unsigned long build_tone(unsigned char *buffer, unsigned long buffer_bytes,
                                const struct test_case *test)
{
    const double two_pi = 6.283185307179586;
    unsigned long tone_frames = (unsigned long)(test->samples_per_second * TONE_SECONDS);
    unsigned long silence_frames = (unsigned long)(test->samples_per_second * SILENCE_SECONDS);
    unsigned long total_frames = tone_frames + silence_frames;
    unsigned int bytes_per_frame = (test->bits_per_sample / 8) * test->channel_count;
    unsigned long needed_bytes = total_frames * bytes_per_frame;
    double radians_per_frame = two_pi * test->tone_hertz / (double)test->samples_per_second;
    unsigned long frame_index;
    unsigned long write_offset = 0;

    if (needed_bytes > buffer_bytes) {
        return 0;
    }

    for (frame_index = 0; frame_index < total_frames; ++frame_index) {
        double wave_value = 0.0;
        unsigned int channel_index;

        if (frame_index < tone_frames) {
            wave_value = sin(radians_per_frame * (double)frame_index) * AMPLITUDE_FRACTION;
        }

        for (channel_index = 0; channel_index < test->channel_count; ++channel_index) {
            if (test->bits_per_sample == 8) {
                int sample = EIGHT_BIT_CENTRE + (int)(wave_value * EIGHT_BIT_PEAK);

                buffer[write_offset] = (unsigned char)sample;
                write_offset += 1;
            } else {
                int sample = (int)(wave_value * SIXTEEN_BIT_PEAK);

                buffer[write_offset] = (unsigned char)(sample & 0xff);
                buffer[write_offset + 1] = (unsigned char)((sample >> 8) & 0xff);
                write_offset += 2;
            }
        }
    }
    return write_offset;
}

static int play_one_case(const struct test_case *test, unsigned char *buffer, unsigned long buffer_bytes)
{
    WAVEFORMATEX wave_format;
    WAVEHDR wave_header;
    HWAVEOUT wave_device = NULL;
    MMRESULT result;
    unsigned long tone_bytes;
    unsigned int waited_milliseconds = 0;

    report("%5u Hz  %2u Bit  %s  Ton %4u Hz  ",
           test->samples_per_second, test->bits_per_sample,
           test->channel_count == 1 ? "mono  " : "stereo",
           test->tone_hertz);

    tone_bytes = build_tone(buffer, buffer_bytes, test);
    if (tone_bytes == 0) {
        report("-> Puffer zu klein\n");
        return 0;
    }

    memset(&wave_format, 0, sizeof(wave_format));
    wave_format.wFormatTag = WAVE_FORMAT_PCM;
    wave_format.nChannels = (WORD)test->channel_count;
    wave_format.nSamplesPerSec = test->samples_per_second;
    wave_format.wBitsPerSample = (WORD)test->bits_per_sample;
    wave_format.nBlockAlign = (WORD)((test->bits_per_sample / 8) * test->channel_count);
    wave_format.nAvgBytesPerSec = wave_format.nSamplesPerSec * wave_format.nBlockAlign;
    wave_format.cbSize = 0;

    result = waveOutOpen(&wave_device, WAVE_MAPPER, &wave_format, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        report("-> waveOutOpen: %s (%u)\n", wave_error_text(result), (unsigned int)result);
        return 0;
    }

    memset(&wave_header, 0, sizeof(wave_header));
    wave_header.lpData = (LPSTR)buffer;
    wave_header.dwBufferLength = tone_bytes;

    result = waveOutPrepareHeader(wave_device, &wave_header, sizeof(wave_header));
    if (result != MMSYSERR_NOERROR) {
        report("-> waveOutPrepareHeader: %s\n", wave_error_text(result));
        waveOutClose(wave_device);
        return 0;
    }

    result = waveOutWrite(wave_device, &wave_header, sizeof(wave_header));
    if (result != MMSYSERR_NOERROR) {
        report("-> waveOutWrite: %s\n", wave_error_text(result));
        waveOutUnprepareHeader(wave_device, &wave_header, sizeof(wave_header));
        waveOutClose(wave_device);
        return 0;
    }

    /* No callback and no event: polling the done flag keeps this C89 and small. */
    while ((wave_header.dwFlags & WHDR_DONE) == 0) {
        Sleep(WAIT_POLL_MILLISECONDS);
        waited_milliseconds += WAIT_POLL_MILLISECONDS;
        if (waited_milliseconds > WAIT_LIMIT_SECONDS * 1000) {
            report("-> Zeitueberschreitung nach %u s\n", WAIT_LIMIT_SECONDS);
            waveOutReset(wave_device);
            waveOutUnprepareHeader(wave_device, &wave_header, sizeof(wave_header));
            waveOutClose(wave_device);
            return 0;
        }
    }

    waveOutUnprepareHeader(wave_device, &wave_header, sizeof(wave_header));
    waveOutClose(wave_device);

    report("-> gespielt, %lu Byte in %u ms\n", tone_bytes, waited_milliseconds);
    return 1;
}

int main(int argc, char **argv)
{
    /* One second of 44100 Hz stereo 16 bit plus the trailing silence, rounded up. */
    const unsigned long buffer_bytes = 44100UL * 2UL * 2UL * 2UL;
    unsigned char *buffer;
    unsigned int case_index;
    unsigned int played_count = 0;
    int list_only = (argc > 1 && strcmp(argv[1], "-list") == 0);

    report_file = fopen(REPORT_FILE_NAME, "w");

    report("sinetest -- Klangpruefung fuer qemu-3dfx\n");
    report("----------------------------------------\n");
    list_devices();
    report("\n");

    if (list_only) {
        if (report_file != NULL) {
            fclose(report_file);
        }
        return 0;
    }

    buffer = (unsigned char *)malloc(buffer_bytes);
    if (buffer == NULL) {
        report("Kein Speicher fuer den Tonpuffer (%lu Byte).\n", buffer_bytes);
        if (report_file != NULL) {
            fclose(report_file);
        }
        return 1;
    }

    for (case_index = 0; case_index < TEST_CASE_COUNT; ++case_index) {
        played_count += play_one_case(&test_cases[case_index], buffer, buffer_bytes);
    }

    report("\n%u von %u Prueffaellen gespielt.\n", played_count, (unsigned int)TEST_CASE_COUNT);
    report("Bericht: %s\n", REPORT_FILE_NAME);

    free(buffer);
    if (report_file != NULL) {
        fclose(report_file);
    }
    return played_count == TEST_CASE_COUNT ? 0 : 1;
}
