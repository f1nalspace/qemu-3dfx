/*
 * glidepal -- Glide test case for palettized textures and the chroma key.
 *
 * Background: Diablo II draws its menu font through GR_TEXFMT_P_8 textures and
 * gets the transparency from the chroma key. Under qemu-3dfx the transparent
 * parts came out as black boxes. This test case pins down where that happens:
 * it draws the same palettized texture four times, each time with a different
 * combination of chroma key and alpha blending, so a single screenshot tells
 * which of the four paths is broken.
 *
 * The same source runs natively on the host against OpenGLide and inside the
 * guest against the qemu-3dfx wrapper. If the host picture is right and the
 * guest picture is wrong, the fault is in the passthrough; if both are wrong,
 * it is in OpenGLide.
 *
 * Written in C89 so that Visual C++ 6.0 inside the guest can compile it too.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "glidemin.h"

#if defined(_WIN32) || defined(__MINGW32__) || defined(_MSC_VER)
#  include <windows.h>
static double seconds_now(void)
{
    return (double)GetTickCount() / 1000.0;
}
#else
#  include <sys/time.h>
static double seconds_now(void)
{
    struct timeval now;
    gettimeofday(&now, 0);
    return (double)now.tv_sec + (double)now.tv_usec / 1000000.0;
}
#endif

/* ------------------------------------------------------------- texture -- */

#define TEXTURE_SIZE      64
#define PALETTE_ENTRIES   256

/* The palette index that is meant to be transparent. Index 0 with a black
 * entry is what Diablo II uses, so the test uses the same. */
#define KEY_INDEX         0
#define KEY_COLOUR        0x00000000

static FxU8  texture_pixels[TEXTURE_SIZE * TEXTURE_SIZE];
static FxU32 palette[PALETTE_ENTRIES];

/*
 * A glyph-like pattern: a ring, a bar across it and a filled corner. Every
 * texel that is meant to show the background gets the key index, everything
 * else one of four solid colours. That way a wrong result is unmistakable --
 * the key index is black, so a broken chroma key shows a black box.
 */
static void build_texture(void)
{
    const int   centre     = TEXTURE_SIZE / 2;
    const float ring_outer = 26.0f;
    const float ring_inner = 17.0f;
    const int   bar_top    = 28;
    const int   bar_bottom = 36;
    const int   corner     = 12;

    int x, y, index;

    for (y = 0; y < TEXTURE_SIZE; y++) {
        for (x = 0; x < TEXTURE_SIZE; x++) {
            float dx       = (float)(x - centre) + 0.5f;
            float dy       = (float)(y - centre) + 0.5f;
            float distance = (float)sqrt(dx * dx + dy * dy);

            index = KEY_INDEX;

            if (distance <= ring_outer && distance >= ring_inner) {
                index = (x < centre) ? 1 : 2;
            }
            if (y >= bar_top && y < bar_bottom && x > 8 && x < TEXTURE_SIZE - 8) {
                index = 3;
            }
            if (x < corner && y < corner) {
                index = 4;
            }

            texture_pixels[y * TEXTURE_SIZE + x] = (FxU8)index;
        }
    }
}

/* Palette entries are 0x00RRGGBB -- Glide ignores the top byte. */
static void build_palette(void)
{
    int entry;

    for (entry = 0; entry < PALETTE_ENTRIES; entry++) {
        palette[entry] = KEY_COLOUR;
    }
    palette[1] = 0x00FF4040;   /* red    */
    palette[2] = 0x0040FF40;   /* green  */
    palette[3] = 0x004080FF;   /* blue   */
    palette[4] = 0x00FFFFFF;   /* white  */
}

/* --------------------------------------------------------------- draw ---- */

static void fill_vertex(GrVertex *vertex, float x, float y, float s, float t,
                        float red, float green, float blue, float alpha)
{
    memset(vertex, 0, sizeof(*vertex));

    vertex->x   = x;
    vertex->y   = y;
    vertex->z   = 0.0f;
    vertex->r   = red;
    vertex->g   = green;
    vertex->b   = blue;
    vertex->a   = alpha;
    vertex->oow = 1.0f;
    vertex->ooz = 1.0f;

    /* Glide texture coordinates run from 0 to 255 across the whole texture,
     * no matter how large it is. They are handed over divided by w. */
    vertex->tmuvtx[0].sow = s * vertex->oow;
    vertex->tmuvtx[0].tow = t * vertex->oow;
    vertex->tmuvtx[0].oow = vertex->oow;
}

static void draw_quad(float left, float top, float size,
                      int textured,
                      float red, float green, float blue, float alpha)
{
    const float texture_span = 255.0f;

    GrVertex corner[4];
    float    right  = left + size;
    float    bottom = top + size;
    float    s      = textured ? texture_span : 0.0f;

    fill_vertex(&corner[0], left,  top,    0.0f, 0.0f, red, green, blue, alpha);
    fill_vertex(&corner[1], right, top,    s,    0.0f, red, green, blue, alpha);
    fill_vertex(&corner[2], right, bottom, s,    s,    red, green, blue, alpha);
    fill_vertex(&corner[3], left,  bottom, 0.0f, s,    red, green, blue, alpha);

    grDrawTriangle(&corner[0], &corner[1], &corner[2]);
    grDrawTriangle(&corner[0], &corner[2], &corner[3]);
}

/* --------------------------------------------------------------- panels -- */

/*
 * Four panels, from left to right. Every one draws an orange plate first and
 * the palettized texture on top of it. Wherever the transparency works, the
 * orange shows through; where it does not, a black box stands there.
 */
enum {
    PANEL_KEY_NO_BLEND = 0,   /* chroma key on,  no blending   -- Diablo II's font case */
    PANEL_NO_KEY       = 1,   /* chroma key off, no blending   -- the control: must be a black box */
    PANEL_KEY_BLEND    = 2,   /* chroma key on,  blending on, alpha iterated */
    PANEL_TEX_ALPHA    = 3,   /* chroma key on,  blending on, alpha out of the texture */
    PANEL_COUNT        = 4
};

static const char *panel_name[PANEL_COUNT] = {
    "1 Chroma-Key an,  keine Mischung  (der Fall von Diablo II)",
    "2 Chroma-Key aus, keine Mischung  (Gegenprobe: muss schwarz sein)",
    "3 Chroma-Key an,  Mischung an, Alpha interpoliert",
    "4 Chroma-Key an,  Mischung an, Alpha aus der Textur"
};

static void set_panel_state(int panel)
{
    grChromakeyValue(KEY_COLOUR);
    grChromakeyMode((panel == PANEL_NO_KEY) ? GR_CHROMAKEY_DISABLE : GR_CHROMAKEY_ENABLE);

    if (panel == PANEL_KEY_NO_BLEND || panel == PANEL_NO_KEY) {
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
    } else {
        grAlphaBlendFunction(GR_BLEND_SRC_ALPHA, GR_BLEND_ONE_MINUS_SRC_ALPHA,
                             GR_BLEND_ONE, GR_BLEND_ZERO);
    }

    if (panel == PANEL_TEX_ALPHA) {
        grAlphaCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
    } else {
        grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
    }
}

static void draw_scene(float panel_size, float panel_top, float gap)
{
    const float plate_border = 8.0f;
    const float plate_red    = 255.0f;
    const float plate_green  = 150.0f;
    const float plate_blue   = 40.0f;

    int   panel;
    float left;

    for (panel = 0; panel < PANEL_COUNT; panel++) {
        left = gap + (float)panel * (panel_size + gap);

        /* the plate underneath, untextured */
        grChromakeyMode(GR_CHROMAKEY_DISABLE);
        grAlphaBlendFunction(GR_BLEND_ONE, GR_BLEND_ZERO, GR_BLEND_ONE, GR_BLEND_ZERO);
        grColorCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
        grAlphaCombine(GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_NONE, FXFALSE);
        draw_quad(left - plate_border, panel_top - plate_border,
                  panel_size + 2.0f * plate_border, 0,
                  plate_red, plate_green, plate_blue, 255.0f);

        /* the palettized texture on top of it */
        set_panel_state(panel);
        grColorCombine(GR_COMBINE_FUNCTION_SCALE_OTHER, GR_COMBINE_FACTOR_ONE,
                       GR_COMBINE_LOCAL_ITERATED, GR_COMBINE_OTHER_TEXTURE, FXFALSE);
        draw_quad(left, panel_top, panel_size, 1, 255.0f, 255.0f, 255.0f, 255.0f);
    }
}

/* ----------------------------------------------------------------- main -- */

int main(int argc, char **argv)
{
    const float panel_size = 120.0f;
    const float panel_top  = 180.0f;
    const float gap        = 30.0f;

    GrHwConfiguration hw;
    GrTexInfo   texture_info;
    FxU32       texture_address;
    double      run_seconds = 20.0;
    double      start_time;
    long        frames = 0;
    int         panel;
    int         argument;

    for (argument = 1; argument < argc; argument++) {
        if (argument == 1) {
            run_seconds = atof(argv[argument]);
        }
    }

    printf("glidepal -- palettierte Textur und Chroma-Key ueber Glide\n");
    for (panel = 0; panel < PANEL_COUNT; panel++) {
        printf("  Feld %s\n", panel_name[panel]);
    }
    printf("Erwartet: Feld 1, 3 und 4 zeigen den orangenen Grund durch,\n");
    printf("          nur Feld 2 zeigt einen schwarzen Kasten.\n");
    fflush(stdout);

    build_texture();
    build_palette();

    grGlideInit();

    memset(&hw, 0, sizeof(hw));
    if (!grSstQueryHardware(&hw)) {
        printf("FEHLER: grSstQueryHardware meldet keine Hardware.\n");
        grGlideShutdown();
        return 1;
    }
    grSstSelect(0);

    if (!grSstWinOpen(0, GR_RESOLUTION_640x480, GR_REFRESH_60Hz,
                      GR_COLORFORMAT_ARGB, GR_ORIGIN_UPPER_LEFT, 2, 1)) {
        printf("FEHLER: grSstWinOpen gescheitert.\n");
        grGlideShutdown();
        return 1;
    }

    grRenderBuffer(GR_BUFFER_BACKBUFFER);
    grCullMode(GR_CULL_DISABLE);
    grColorMask(FXTRUE, FXTRUE);
    grDepthBufferMode(GR_DEPTHBUFFER_DISABLE);
    grDepthMask(FXFALSE);

    /* one TMU, the texel goes through unchanged */
    grTexCombine(GR_TMU0,
                 GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 GR_COMBINE_FUNCTION_LOCAL, GR_COMBINE_FACTOR_NONE,
                 FXFALSE, FXFALSE);
    grTexFilterMode(GR_TMU0, GR_TEXTUREFILTER_POINT_SAMPLED, GR_TEXTUREFILTER_POINT_SAMPLED);
    grTexClampMode(GR_TMU0, GR_TEXTURECLAMP_CLAMP, GR_TEXTURECLAMP_CLAMP);
    grTexMipMapMode(GR_TMU0, GR_MIPMAP_DISABLE, FXFALSE);

    texture_info.smallLod    = GR_LOD_64;
    texture_info.largeLod    = GR_LOD_64;
    texture_info.aspectRatio = GR_ASPECT_1x1;
    texture_info.format      = GR_TEXFMT_P_8;
    texture_info.data        = texture_pixels;

    texture_address = grTexMinAddress(GR_TMU0);
    grTexDownloadMipMap(GR_TMU0, texture_address, GR_MIPMAPLEVELMASK_BOTH, &texture_info);
    grTexDownloadTable(GR_TMU0, GR_TEXTABLE_PALETTE, palette);
    grTexSource(GR_TMU0, texture_address, GR_MIPMAPLEVELMASK_BOTH, &texture_info);

    printf("Textur %dx%d als GR_TEXFMT_P_8 auf Adresse %lu geladen.\n",
           TEXTURE_SIZE, TEXTURE_SIZE, (unsigned long)texture_address);
    fflush(stdout);

    start_time = seconds_now();
    for (;;) {
        double now;

        grBufferClear(0xFF102030, 255, GR_WDEPTHVALUE_FARTHEST);
        draw_scene(panel_size, panel_top, gap);
        grBufferSwap(0);
        frames++;

        now = seconds_now();
        if (run_seconds > 0.0 && (now - start_time) >= run_seconds) {
            break;
        }
    }

    printf("%ld Bilder gezeichnet.\n", frames);
    fflush(stdout);

    grSstWinClose();
    grGlideShutdown();
    return 0;
}
