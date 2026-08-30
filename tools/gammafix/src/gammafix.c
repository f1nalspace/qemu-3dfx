/*
 * gammafix -- read back and restore the gamma ramp of the X screen.
 *
 * Why it exists: qemu-3dfx forwards the guest's SetDeviceGammaRamp to the host's
 * XF86VidModeSetGammaRamp (hw/mesa/mglcntx_linux.c). That covers the entire X
 * screen, not just the QEMU window. If the guest program crashes before
 * MGLWndRelease() puts the ramp back, the host desktop stays in the game's ramp
 * -- unreadably dark in the worst case.
 *
 * No argument: show the ramp. With "--reset": write a linear ramp. The identity
 * ramp is bit for bit the one MesaInitGammaRamp() writes.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/extensions/xf86vmode.h>

static const int gammaRampSize256 = 0x100;
static const int gammaRampSize1024 = 0x400;
static const int gammaRampSize2048 = 0x800;

/* Bit replication, the same as MesaInitGammaRamp() in mglcntx_linux.c: a ramp
   index becomes a 16-bit value whose low bits repeat the high ones. */
static unsigned short IdentityRampValue(int index, int rampSize)
{
    if (rampSize == gammaRampSize256)
        return (unsigned short)(((index << 8) | index) & 0xFFFF);
    if (rampSize == gammaRampSize1024) {
        int scaled = index << 6;
        return (unsigned short)((scaled | ((scaled & 0xFC00) >> 10)) & 0xFFFF);
    }
    int scaled = index << 5;
    return (unsigned short)((scaled | ((scaled & 0xF800) >> 11)) & 0xFFFF);
}

int main(int argc, char **argv)
{
    int wantReset = (argc > 1) && !strcmp(argv[1], "--reset");

    Display *display = XOpenDisplay(NULL);
    if (!display) {
        fprintf(stderr, "gammafix: kein X-Display erreichbar (DISPLAY gesetzt?)\n");
        return 1;
    }
    int screen = DefaultScreen(display);

    int eventBase, errorBase;
    if (!XF86VidModeQueryExtension(display, &eventBase, &errorBase)) {
        fprintf(stderr, "gammafix: XF86VidMode-Erweiterung fehlt\n");
        XCloseDisplay(display);
        return 1;
    }

    int rampSize = 0;
    XF86VidModeGetGammaRampSize(display, screen, &rampSize);
    if (rampSize != gammaRampSize256 && rampSize != gammaRampSize1024 && rampSize != gammaRampSize2048) {
        fprintf(stderr, "gammafix: unerwartete Rampengroesse %d\n", rampSize);
        XCloseDisplay(display);
        return 1;
    }

    unsigned short *red = calloc(rampSize, sizeof(unsigned short));
    unsigned short *green = calloc(rampSize, sizeof(unsigned short));
    unsigned short *blue = calloc(rampSize, sizeof(unsigned short));

    if (wantReset) {
        for (int i = 0; i < rampSize; i++) {
            unsigned short value = IdentityRampValue(i, rampSize);
            red[i] = value;
            green[i] = value;
            blue[i] = value;
        }
        XF86VidModeSetGammaRamp(display, screen, rampSize, red, green, blue);
        XSync(display, False);
        printf("gammafix: lineare Rampe geschrieben, %d Stufen\n", rampSize);
    }
    else {
        XF86VidModeGetGammaRamp(display, screen, rampSize, red, green, blue);
        printf("Rampengroesse: %d\n", rampSize);

        int largestDeviation = 0;
        for (int i = 0; i < rampSize; i++) {
            int expected = IdentityRampValue(i, rampSize);
            int deviation = (int)red[i] - expected;
            if (deviation < 0) deviation = -deviation;
            if (deviation > largestDeviation) largestDeviation = deviation;
        }
        printf("Groesste Abweichung von der Identitaet: %d von 65535\n", largestDeviation);

        const int sampleCount = 5;
        for (int s = 0; s < sampleCount; s++) {
            int index = (rampSize - 1) * s / (sampleCount - 1);
            printf("  [%4d] r=%5u g=%5u b=%5u   (linear waere %5u)\n",
                index, red[index], green[index], blue[index], IdentityRampValue(index, rampSize));
        }
        if (largestDeviation > 256)
            printf("\nDie Rampe ist NICHT linear. Mit \"gammafix --reset\" zuruecksetzen.\n");
    }

    free(red);
    free(green);
    free(blue);
    XCloseDisplay(display);
    return 0;
}
