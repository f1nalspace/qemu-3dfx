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
 * Usage:  glidecube [-seconds N] [-width N] [-info] [-vsync]
 *         -seconds N  run time, default 15. 0 means forever.
 *         -width N    320, 512, 640 or 800. Default 640.
 *         -info       print the hardware details only, draw nothing.
 *         -vsync      wait for vertical blank. Without this switch the frame
 *                     rate measures throughput rather than the refresh rate of
 *                     the display.
 *         A bare number still works: the first is the run time, the second the
 *         width. Anything else that starts with a dash is refused -- silently
 *         reading it as a number is how "-seconds 6" once meant "run forever",
 *         and a Glide run has no window to close (docs/LOG.md [424]).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "glidemin.h"

/* The transparency test. Glide is the interesting one: it has two ways to make a
 * texel vanish -- alpha blending and the chroma key -- and the chroma key was
 * wrong twice in OpenGLide, both times found the hard way at Diablo II's menu
 * (docs/LOG.md [236]-[239]). Headers come from tools/common of the project tree,
 * $(KEYTEST) in the Makefile says where.
 */
#include "keytest.h"
#include "keylogo_glide.h"
#include "logo_3dfx.h"

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

/* ------------------------------------------------------------ interrupt -- */

/* Glide draws on the host, not into a guest window, so there is nothing to click
 * shut and no message queue to close. Without this the only way out of a run is
 * Ctrl+C in the console, and that ends the process hard -- which leaves the
 * wrapper's use count above zero and blocks Glide for the rest of the Windows
 * session (wrappers/3dfx/src/gl301dll.c:981). Esc is asynchronous, so it does not
 * matter which window holds the focus.
 */
static int stop_requested(void)
{
#if defined(_WIN32) || defined(__MINGW32__)
    return (GetAsyncKeyState(VK_ESCAPE) & 0x8000)? 1:0;
#else
    return 0;
#endif
}

static void print_usage(const char *program_name)
{
    printf("Usage: %s [-seconds N] [-width N] [-info] [-vsync]\n", program_name);
    printf("  -seconds N  run time in seconds, default 15. 0 means endless.\n");
    printf("  -width N    320, 512, 640 or 800. Default 640.\n");
    printf("  -info       print the device data only, draw nothing.\n");
    printf("  -vsync      wait for the vertical retrace.\n");
    printf("  Esc ends a running pass.\n");
    fflush(stdout);
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
    default:                 return "unknown";
    }
}

static void report_hardware(const GrHwConfiguration *hw)
{
    int board;

    printf("grSstQueryHardware: %d card(s)\n", hw->num_sst);
    for (board = 0; board < hw->num_sst && board < MAX_NUM_SST; board++) {
        GrSstType type = hw->SSTs[board].type;
        printf("  Card %d: %s\n", board, sst_type_name(type));
        if (type == GR_SSTTYPE_VOODOO || type == GR_SSTTYPE_Voodoo2) {
            const GrVoodooConfig_t *cfg = &hw->SSTs[board].sstBoard.VoodooConfig;
            printf("    Frame buffer %d MB, Pixelfx rev %d, %d Texelfx, SLI %s\n",
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
    int positional_arguments = 0;
    int stopped_by_key = 0;
    int swap_interval = 0;   /* 0 = do not wait for vertical blank */
    int argument;
    double start_time, last_report, now;
    long frames_total = 0, frames_since_report = 0;
    KeyLogoGlide transparency_logo;
    int transparency_alpha_passed = 0;
    int transparency_chroma_passed = 0;
    float angle = 0.0f;
    const double rotations_per_second = 0.12;

    for (argument = 1; argument < argc; argument++) {
        if (strcmp(argv[argument], "-info") == 0) {
            info_only = 1;
        } else if (strcmp(argv[argument], "-vsync") == 0) {
            swap_interval = 1;
        } else if (strcmp(argv[argument], "-seconds") == 0 && argument + 1 < argc) {
            run_seconds = atof(argv[++argument]);
        } else if (strcmp(argv[argument], "-width") == 0 && argument + 1 < argc) {
            requested_width = atoi(argv[++argument]);
        } else if (argv[argument][0] == '-') {
            printf("Unknown option: %s\n", argv[argument]);
            print_usage(argv[0]);
            return 2;
        } else if (positional_arguments++ == 0) {
            run_seconds = atof(argv[argument]);
        } else {
            requested_width = atoi(argv[argument]);
        }
    }

    printf("glidecube -- Glide test case for qemu-3dfx\n");
    fflush(stdout);

    grGlideInit();

    memset(&hw, 0, sizeof(hw));
    if (!grSstQueryHardware(&hw)) {
        printf("ERROR: grSstQueryHardware reports no hardware.\n");
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
        printf("ERROR: grSstWinOpen failed.\n");
        grGlideShutdown();
        return 1;
    }
    printf("Window open: %d x %d\n", (int)screen_width, (int)screen_height);
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

    keylogo_glide_create(&transparency_logo, logo_3dfx_rgba,
                         LOGO_3DFX_WIDTH, LOGO_3DFX_HEIGHT, LOGO_3DFX_SOLID_PIXELS,
                         (int)screen_width, (int)screen_height, 0x20, 0x20, 0x20);

    start_time  = seconds_now();
    last_report = start_time;

    for (;;) {
        /* With GR_COLORFORMAT_ARGB the top byte is alpha. At 0 the background
         * turns transparent as soon as the host compositor looks at the alpha
         * channel. So 0xFF. */
        grBufferClear(0xFF202020, 255, GR_WDEPTHVALUE_FARTHEST);
        draw_cube(angle);
        /* Alternates between the two routes so both stay visible while it runs;
         * the two checks below each look at the frame that used their own route.
         */
        keylogo_glide_draw(&transparency_logo, (frames_total & 1) ? 1 : 0, 0x20, 0x20, 0x20);

        if (frames_total == 2)
            transparency_alpha_passed = keylogo_glide_verify(&transparency_logo, "alpha blending");
        else if (frames_total == 3)
            transparency_chroma_passed = keylogo_glide_verify(&transparency_logo, "chroma key");

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
        if (stop_requested()) {
            stopped_by_key = 1;
            break;
        }
    }

    now = seconds_now();
    if (stopped_by_key)
        printf("Cancelled with Esc.\n");
    printf("total: %ld frames in %.1f seconds, %.1f FPS\n",
           frames_total, now - start_time,
           frames_total / (now - start_time));
    fflush(stdout);

    printf("Transparency through Glide, alpha blending: %s\n",
           transparency_logo.usable ? (transparency_alpha_passed ? "fine" : "BROKEN")
                                    : "not tested");
    printf("Transparency through Glide, chroma key    : %s\n",
           transparency_logo.usable ? (transparency_chroma_passed ? "fine" : "BROKEN")
                                    : "not tested");
    fflush(stdout);

    keylogo_glide_release(&transparency_logo);
    grSstWinClose();
    grGlideShutdown();
    return 0;
}
