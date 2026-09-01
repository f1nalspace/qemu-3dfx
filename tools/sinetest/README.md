# sinetest — a measurable check on QEMU's sound device models

Plays one sine tone per PCM format through `waveOut`, each at its own pitch and
separated by silence. Six cases in one run, about eight seconds:

| Rate | Bits | Channels | Tone |
|---|---|---|---|
| 11025 | 8 | mono | 400 Hz |
| 11025 | 16 | stereo | 600 Hz |
| 22050 | 8 | mono | 800 Hz |
| 22050 | 16 | stereo | 1000 Hz |
| 44100 | 8 | mono | 1200 Hz |
| 44100 | 16 | stereo | 1400 Hz |

## Why the pitches differ

A sound device model that mistakes the sample rate does not go silent — it plays
at the wrong speed, and the pitch shifts by exactly the ratio of the two rates.
That is a number, not an opinion. Because every case carries its own pitch, a
recording taken on the host splits back into labelled segments: the tone that
comes out at 200 Hz instead of 800 Hz says "the model used 5512 Hz where the
guest asked for 22050".

That is precisely the failure this was written for. QEMU's ES1370 dropped every
register access narrower than 32 bits, so `CTRL_WTSRSEL` never left its reset
value and DAC1 stayed at 5512 Hz — four times too slow, which the ear reports as
a buzz.

## Building and running

    make                        # -> build/SINETEST.EXE

    SINETEST.EXE                # plays all six cases
    SINETEST.EXE -list          # enumerates the playback devices, plays nothing

The report goes to the console and to `C:\SINEOUT.TXT`, so it can be read back
from the host with `guestfish` instead of off a screenshot.

## Recording it on the host

Start QEMU with the wav backend instead of a live one, then check the file:

    -audiodev wav,id=snd0,path=out.wav

The four rates are the ones an ES1370 DAC1 clocks natively (5512, 11025, 22050,
44100); DAC2 derives its rate from a divider and takes anything.
