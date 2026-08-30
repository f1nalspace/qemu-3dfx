/*
 * glidecube -- a small test case for the Glide path of qemu-3dfx.
 *
 * The counterpart to wglgears for OpenGL: a rotating, Gouraud-shaded cube with
 * a depth buffer, plus the frame rate and whatever grSstQueryHardware reports.
 *
 * Deliberately plain C89 with no library beyond Glide and the C runtime. That
 * way it compiles with gcc against OpenGLide on the host, with
 * i686-w64-mingw32-gcc for the guest, and with Visual C++ 6.0 inside the guest.
 *
 * Usage:  glidecube [seconds] [resolution]
 *         seconds     run time, default 15. 0 means forever.
 *         resolution  320, 512, 640 or 800. Default 640.
 *         -info       print the hardware details only, draw nothing.
 *         -vsync      wait for vertical blank. Without this switch the frame
 *                     rate measures throughput rather than the refresh rate of
 *                     the display.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "glidemin.h"

#if defined(_WIN32) || defined(__MINGW32__)
#  include <windows.h>
#else
#  include <sys/time.h>
#endif

/* ------------------------------------------------------------------ time -- */

static double seconds_now(void)
{
#if defined(_WIN32) || defined(__MINGW32__)
    return (double)GetTickCount() / 1000.0;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
#endif
}

/* -------------------------------------------------------------- geometry -- */

/* A cube around the origin. Every face gets its own colour and the corners are
 * shaded slightly differently, so that the Gouraud shading becomes visible --
 * a flat-coloured face would not reveal whether anything is interpolated. */

static const float cube_corner[8][3] = {
    { -1.0f, -1.0f, -1.0f }, {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f }, { -1.0f,  1.0f, -1.0f },
    { -1.0f, -1.0f,  1.0f }, {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f }, { -1.0f,  1.0f,  1.0f }
};

/* six faces, four corners each, counter-clockwise seen from outside */
static const int cube_face[6][4] = {
    { 0, 1, 2, 3 },   /* back   */
    { 5, 4, 7, 6 },   /* front  */
    { 4, 0, 3, 7 },   /* left   */
    { 1, 5, 6, 2 },   /* right  */
    { 4, 5, 1, 0 },   /* bottom */
    { 3, 2, 6, 7 }    /* top    */
};

static const float face_colour[6][3] = {
    { 220.0f,  60.0f,  60.0f },
    {  60.0f, 220.0f,  60.0f },
    {  60.0f,  60.0f, 220.0f },
    { 220.0f, 220.0f,  60.0f },
    { 220.0f,  60.0f, 220.0f },
    {  60.0f, 220.0f, 220.0f }
};

/* --------------------------------------------------------------- drawing -- */

static float screen_width  = 640.0f;
static float screen_height = 480.0f;

/*
 * A point is rotated, pushed back and divided perspectively. Glide expects
 * finished screen coordinates -- there are no matrices and no transform stage,
 * that is the program's job.
 */
static void project_vertex(GrVertex *out,
                           const float corner[3],
                           const float colour[3],
                           float shade,
                           float sin_x, float cos_x,
                           float sin_y, float cos_y)
{
    const float camera_distance = 4.5f;
    const float focal_length    = 1.6f;

    float x0, y0, z0, x1, y1, z1, x2, y2, z2;
    float half_width, half_height, scale, w;

    x0 = corner[0];
    y0 = corner[1];
    z0 = corner[2];

    /* rotate around the y axis, then around the x axis */
    x1 =  x0 * cos_y + z0 * sin_y;
    z1 = -x0 * sin_y + z0 * cos_y;
    y1 =  y0;

    y2 =  y1 * cos_x - z1 * sin_x;
    z2 =  y1 * sin_x + z1 * cos_x;
    x2 =  x1;

    w = z2 + camera_distance;
    if (w < 0.1f) {
        w = 0.1f;
    }

    half_width  = screen_width  * 0.5f;
    half_height = screen_height * 0.5f;
    scale       = half_height * focal_length;

    out->x = half_width  + x2 * scale / w;
    out->y = half_height - y2 * scale / w;   /* origin is at the top left */
    out->z = 0.0f;

    out->r = colour[0] * shade;
    out->g = colour[1] * shade;
    out->b = colour[2] * shade;
    out->a = 255.0f;

    out->oow = 1.0f / w;
    out->ooz = 65535.0f * out->oow;

    memset(out->tmuvtx, 0, sizeof(out->tmuvtx));
}

static void draw_cube(float angle)
{
    /* The four corners of a face get slightly different brightness, so that
     * the interpolation across the face becomes visible. */
    static const float corner_shade[4] = { 1.0f, 0.75f, 0.5f, 0.75f };

    float sin_x, cos_x, sin_y, cos_y;
    int face, corner;
    GrVertex vertex[4];

    sin_x = (float)sin(angle * 0.7);
    cos_x = (float)cos(angle * 0.7);
    sin_y = (float)sin(angle);
    cos_y = (float)cos(angle);

    for (face = 0; face < 6; face++) {
        for (corner = 0; corner < 4; corner++) {
            project_vertex(&vertex[corner],
                           cube_corner[cube_face[face][corner]],
                           face_colour[face],
                           corner_shade[corner],
                           sin_x, cos_x, sin_y, cos_y);
        }
        /* The quad as two triangles. Back faces are not culled, the depth
         * buffer takes care of that. */
        grDrawTriangle(&vertex[0], &vertex[1], &vertex[2]);
        grDrawTriangle(&vertex[0], &vertex[2], &vertex[3]);
    }
}

/* ------------------------------------------------------------- hardware -- */

static const char * sst_type_name(GrSstType type)
{
    switch (type) {
    case GR_SSTTYPE_VOODOO:  return "Voodoo Graphics";
    case GR_SSTTYPE_SST96:   return "SST96";
    case GR_SSTTYPE_AT3D:    return "AT3D";
    case GR_SSTTYPE_Voodoo2: return "Voodoo2";
    default:                 return "unbekannt";
    }
}

static void report_hardware(const GrHwConfiguration *hw)
{
    int board;

    printf("grSstQueryHardware: %d Karte(n)\n", hw->num_sst);
    for (board = 0; board < hw->num_sst && board < MAX_NUM_SST; board++) {
        GrSstType type = hw->SSTs[board].type;
        printf("  Karte %d: %s\n", board, sst_type_name(type));
        if (type == GR_SSTTYPE_VOODOO || type == GR_SSTTYPE_Voodoo2) {
            const GrVoodooConfig_t *cfg = &hw->SSTs[board].sstBoard.VoodooConfig;
            printf("    Bildspeicher %d MB, Pixelfx-Rev %d, %d Texelfx, SLI %s\n",
                   cfg->fbRam, cfg->fbiRev, cfg->nTexelfx,
                   cfg->sliDetect ? "ja" : "nein");
            if (cfg->nTexelfx > 0) {
                printf("    TMU0: Rev %d, %d MB\n",
                       cfg->tmuConfig[0].tmuRev, cfg->tmuConfig[0].tmuRam);
            }
        }
    }
    fflush(stdout);
}

/* ----------------------------------------------------------------- main -- */

static GrScreenResolution_t resolution_from_width(int width)
{
    switch (width) {
    case 320: screen_width = 320.0f; screen_height = 240.0f; return GR_RESOLUTION_320x240;
    case 512: screen_width = 512.0f; screen_height = 384.0f; return GR_RESOLUTION_512x384;
    case 800: screen_width = 800.0f; screen_height = 600.0f; return GR_RESOLUTION_800x600;
    default:  screen_width = 640.0f; screen_height = 480.0f; return GR_RESOLUTION_640x480;
    }
}

int main(int argc, char **argv)
{
    const double report_interval = 5.0;

    GrHwConfiguration hw;
    GrScreenResolution_t resolution;
    double run_seconds = 15.0;
    int requested_width = 640;
    int info_only = 0;
    int swap_interval = 0;   /* 0 = do not wait for vertical blank */
    int argument;
    double start_time, last_report, now;
    long frames_total = 0, frames_since_report = 0;
    float angle = 0.0f;
    const double rotations_per_second = 0.12;

    for (argument = 1; argument < argc; argument++) {
        if (strcmp(argv[argument], "-info") == 0) {
            info_only = 1;
        } else if (strcmp(argv[argument], "-vsync") == 0) {
            swap_interval = 1;
        } else if (argument == 1) {
            run_seconds = atof(argv[argument]);
        } else {
            requested_width = atoi(argv[argument]);
        }
    }

    printf("glidecube -- Glide-Prueffall fuer qemu-3dfx\n");
    fflush(stdout);

    grGlideInit();

    memset(&hw, 0, sizeof(hw));
    if (!grSstQueryHardware(&hw)) {
        printf("FEHLER: grSstQueryHardware meldet keine Hardware.\n");
        grGlideShutdown();
        return 1;
    }
    report_hardware(&hw);

    if (info_only) {
        grGlideShutdown();
        return 0;
    }

    grSstSelect(0);

    resolution = resolution_from_width(requested_width);
    if (!grSstWinOpen(0, resolution, GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        printf("FEHLER: grSstWinOpen gescheitert.\n");
        grGlideShutdown();
        return 1;
    }
    printf("Fenster offen: %d x %d\n", (int)screen_width, (int)screen_height);
    fflush(stdout);

    grRenderBuffer(GR_BUFFER_BACKBUFFER);
    grCullMode(GR_CULL_DISABLE);

    /* In Glide the write mask for the alpha channel defaults to off. On the
     * host OpenGLide picks a GLX visual with eight alpha bits; if the channel
     * stays at zero, the compositor treats the window as transparent. In the
     * guest this has no effect, but does no harm either. */
    grColorMask(FXTRUE, FXTRUE);

    /* Colour and alpha come straight from the interpolation of the corners --
     * no textures, no fog, nothing in between. */
    grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                   GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);

    grDepthBufferMode(GR_DEPTHBUFFER_WBUFFER);
    grDepthBufferFunction(GR_CMP_LESS);
    grDepthMask(FXTRUE);

    start_time  = seconds_now();
    last_report = start_time;

    for (;;) {
        /* With GR_COLORFORMAT_ARGB the top byte is alpha. At 0 the background
         * turns transparent as soon as the host compositor looks at the alpha
         * channel. So 0xFF. */
        grBufferClear(0xFF202020, 255, GR_WDEPTHVALUE_FARTHEST);
        draw_cube(angle);
        grBufferSwap(swap_interval);

        frames_total++;
        frames_since_report++;

        now = seconds_now();
        /* The angle follows elapsed time, not the frame count -- otherwise the
         * cube would spin absurdly fast at 15,000 frames per second and would
         * look different on every machine. */
        angle = (float)((now - start_time) * rotations_per_second * 6.283185307);
        if (now - last_report >= report_interval) {
            double elapsed = now - last_report;
            printf("%ld frames in %.1f seconds, %.1f FPS\n",
                   frames_since_report, elapsed, frames_since_report / elapsed);
            fflush(stdout);
            frames_since_report = 0;
            last_report = now;
        }
        if (run_seconds > 0.0 && (now - start_time) >= run_seconds) {
            break;
        }
    }

    now = seconds_now();
    printf("gesamt: %ld frames in %.1f seconds, %.1f FPS\n",
           frames_total, now - start_time,
           frames_total / (now - start_time));
    fflush(stdout);

    grSstWinClose();
    grGlideShutdown();
    return 0;
}
