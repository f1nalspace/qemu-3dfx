/*
 * timecheck -- do the clocks in the guest still tell the truth once qemu-3dfx has hooked
 * them? Written for docs/LOG.md [380]: Unreal Tournament runs far too fast over Direct3D
 * and OpenGL, but not over Glide, and the suspect is HookPatchTimer() in
 * wrappers/fxlib/fxhook.c -- it redirects timeGetTime() and GetTickCount() of the running
 * application to a QueryPerformanceCounter-derived replacement. On Windows 9x it is
 * switched off, on NT/XP it is not.
 *
 * The measurement needs no reference clock from outside: QueryPerformanceCounter is NOT
 * hooked on NT, so it is the yardstick, and every other clock is counted against it. The
 * same span is measured twice -- once before a GL context exists, once after, because the
 * hook patches the import table when the wrapper comes up.
 *
 * Build:  make            -> build/TIMECHK.EXE
 * Run:    TIMECHK.EXE > C:\TIMECHK.TXT
 *         Put it where the game's opengl32.dll lives, so the same DLL is loaded.
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdio.h>

static const int measuredSeconds = 4;
static const int presentedFrames = 60;
static const double millisecondsPerSecond = 1000.0;

struct clockSample {
    LARGE_INTEGER performanceCounter;
    DWORD multimediaTime;
    DWORD kernelTickCount;
};

static void takeClockSample(struct clockSample *sample)
{
    QueryPerformanceCounter(&sample->performanceCounter);
    sample->multimediaTime = timeGetTime();
    sample->kernelTickCount = GetTickCount();
}

/* Busy-waiting on the performance counter, so the wait itself does not depend on the very
 * clocks under test -- Sleep() rides on the kernel tick.
 */
static void waitOnPerformanceCounter(const LONGLONG counterFrequency, const int seconds)
{
    LARGE_INTEGER start, now;
    LONGLONG target;

    QueryPerformanceCounter(&start);
    target = start.QuadPart + (counterFrequency * seconds);
    do {
        QueryPerformanceCounter(&now);
    } while (now.QuadPart < target);
}

static void measureAndReport(const char *what, const LONGLONG counterFrequency)
{
    struct clockSample before, after;
    double referenceMilliseconds, multimediaMilliseconds, kernelMilliseconds;

    takeClockSample(&before);
    waitOnPerformanceCounter(counterFrequency, measuredSeconds);
    takeClockSample(&after);

    const LONGLONG counterTicks = after.performanceCounter.QuadPart - before.performanceCounter.QuadPart;
    referenceMilliseconds = (counterTicks * millisecondsPerSecond) / counterFrequency;
    multimediaMilliseconds = (double)(after.multimediaTime - before.multimediaTime);
    kernelMilliseconds = (double)(after.kernelTickCount - before.kernelTickCount);

    printf("%s\n", what);
    printf("  QueryPerformanceCounter : %10.1f ms   (Bezug)\n", referenceMilliseconds);
    printf("  timeGetTime             : %10.1f ms   Faktor %.4f\n",
           multimediaMilliseconds, multimediaMilliseconds / referenceMilliseconds);
    printf("  GetTickCount            : %10.1f ms   Faktor %.4f\n",
           kernelMilliseconds, kernelMilliseconds / referenceMilliseconds);
    fflush(stdout);
}

static LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wordParameter, LPARAM longParameter)
{
    return DefWindowProc(window, message, wordParameter, longParameter);
}

/* A GL context is what brings the wrapper up, and with it the hook. Everything here is the
 * shortest path to one; the window is never shown.
 */
static HGLRC createGlContext(HDC *deviceContextOut, HWND *windowOut)
{
    static const char windowClassName[] = "timecheck";
    WNDCLASS windowClass;
    PIXELFORMATDESCRIPTOR pixelFormat;
    HWND window;
    HDC deviceContext;
    int pixelFormatIndex;
    HGLRC renderingContext;

    memset(&windowClass, 0, sizeof(windowClass));
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = GetModuleHandle(NULL);
    windowClass.lpszClassName = windowClassName;
    RegisterClass(&windowClass);

    window = CreateWindowEx(0, windowClassName, "timecheck", WS_OVERLAPPEDWINDOW,
                            0, 0, 320, 240, NULL, NULL, windowClass.hInstance, NULL);
    if (!window) {
        return NULL;
    }
    deviceContext = GetDC(window);

    memset(&pixelFormat, 0, sizeof(pixelFormat));
    pixelFormat.nSize = sizeof(pixelFormat);
    pixelFormat.nVersion = 1;
    pixelFormat.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pixelFormat.iPixelType = PFD_TYPE_RGBA;
    pixelFormat.cColorBits = 32;
    pixelFormat.cDepthBits = 24;

    pixelFormatIndex = ChoosePixelFormat(deviceContext, &pixelFormat);
    if (!pixelFormatIndex) {
        return NULL;
    }
    SetPixelFormat(deviceContext, pixelFormatIndex, &pixelFormat);

    renderingContext = wglCreateContext(deviceContext);
    if (renderingContext) {
        wglMakeCurrent(deviceContext, renderingContext);
    }
    *deviceContextOut = deviceContext;
    *windowOut = window;
    return renderingContext;
}

int main(void)
{
    LARGE_INTEGER counterFrequency;
    HDC deviceContext = NULL;
    HWND window = NULL;
    HGLRC renderingContext;

    if (!QueryPerformanceFrequency(&counterFrequency) || !counterFrequency.QuadPart) {
        printf("Kein Leistungszaehler vorhanden -- Messung nicht moeglich.\n");
        return 1;
    }
    printf("timecheck -- Uhren im Gast, gemessen gegen QueryPerformanceCounter\n");
    printf("Zaehlerfrequenz: %I64d Hz, Messdauer je Durchgang: %d s\n\n",
           counterFrequency.QuadPart, measuredSeconds);

    measureAndReport("Vor dem GL-Kontext (ungehakt):", counterFrequency.QuadPart);

    renderingContext = createGlContext(&deviceContext, &window);
    if (!renderingContext) {
        printf("\nKein GL-Kontext -- der zweite Durchgang faellt aus.\n");
        return 1;
    }
    printf("\nGL-Kontext steht: %s\n", (const char *)glGetString(GL_RENDERER));
    printf("\n");

    measureAndReport("Nach dem GL-Kontext (gehakt, falls der Haken greift):", counterFrequency.QuadPart);

    /* The wrapper patches the import table when it comes up, and it may not consider itself
     * up before the first frame reaches the screen. So: present a few, then measure again.
     */
    printf("\n%d Bildwechsel...\n", presentedFrames);
    int frame;
    for (frame = 0; frame < presentedFrames; frame++) {
        glClear(GL_COLOR_BUFFER_BIT);
        SwapBuffers(deviceContext);
    }
    printf("\n");
    measureAndReport("Nach den Bildwechseln:", counterFrequency.QuadPart);

    printf("\nEin Faktor deutlich ueber 1 heisst: die Uhr laeuft zu schnell, und das Spiel mit ihr.\n");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(renderingContext);
    ReleaseDC(window, deviceContext);
    DestroyWindow(window);
    return 0;
}
