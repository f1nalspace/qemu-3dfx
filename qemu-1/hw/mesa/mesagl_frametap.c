/*
 * QEMU MESA GL Pass-Through -- frametap, the frame rate overlay
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
#include "qemu/timer.h"
#include "qemu/atomic.h"
#include "mesagl_impl.h"
#include "mesagl_frametap.h"
#include "fgfont.h"

int mesa_gui_fullscreen(const void *);

/* What mesa_gui_fullscreen() fills in, the same query the scaler makes. */
enum {
    GUI_SIZE_GUEST_WIDTH,
    GUI_SIZE_GUEST_HEIGHT,
    GUI_SIZE_DRAWABLE_WIDTH,
    GUI_SIZE_DRAWABLE_HEIGHT,
    GUI_SIZE_COUNT,
};

/* The overlay is one quad drawn with a program of its own: the text is an array of glyph numbers in a uniform, the glyphs come from an atlas texture.
 * No framebuffer object and no blit -- on NVIDIA a blit into the window, or even a framebuffer switch, is paid for again in the swap that follows,
 * 27 and 8 µs per frame, against 1.3 µs for the quad (docs/LOG.md [833], [838] in the project repository).
 * The state the quad needs is saved and given back every frame. A text change is one uniform update, four times a second.
 */
#define FRAMETAP_FIRST_GLYPH                        32
#define FRAMETAP_GLYPH_COUNT                        96
#define FRAMETAP_UNKNOWN_GLYPH                      '?'
#define FRAMETAP_GLYPH_WIDTH                        8
#define FRAMETAP_GLYPH_HEIGHT                       14
#define FRAMETAP_LINE_GLYPHS                        24
#define FRAMETAP_WINDOW_SLOTS                       4
#define FRAMETAP_UPDATE_INTERVAL_NS                 (250LL * 1000LL * 1000LL)
/* A guest that swapped within the last second presents with its swaps; its glFlush calls are no frames. */
#define FRAMETAP_SWAP_RECENT_NS                     (1000LL * 1000LL * 1000LL)
#define FRAMETAP_MAX_CONTEXTS                       8
#define FRAMETAP_DRAWABLE_HEIGHT_PER_SCALE_STEP     360
#define FRAMETAP_MARGIN_PIXELS                      8
#define FRAMETAP_BYTES_PER_PIXEL                    4
#define FRAMETAP_QUAD_VERTICES                      4
#define FRAMETAP_VIEWPORT_VALUES                    4
#define FRAMETAP_COLOR_MASK_VALUES                  4
#define FRAMETAP_POLYGON_MODE_VALUES                2
#define FRAMETAP_NANOSECONDS_PER_MICROSECOND        1000.0
#define FRAMETAP_RATE_TO_HUNDREDTHS                 100.0
#define FRAMETAP_ROUND_TO_NEAREST                   0.5
/* fgfont.h puts a width byte in front of each glyph's rows. */
#define FGFONT_FIRST_ROW_OFFSET                     1
#define FRAMETAP_INFO_LOG_SIZE                      512

/* Texture names of a compatibility context need no glGenTextures, and games of the time pick their own from 1 upwards -- GLQuake does.
 * glGenTextures hands out the lowest free name, so neither ever reaches this range. A core context gets generated names instead.
 */
#define FRAMETAP_TEXTURE_NAME_BASE                  0x7fff0000U
#define FRAMETAP_CORE_PROFILE_MIN_MAJOR             3
#define FRAMETAP_CORE_PROFILE_MIN_MINOR             2

/* Pure yellow on black, opaque. */
static const uint8_t frametapTextColor[FRAMETAP_BYTES_PER_PIXEL] = { 0xff, 0xff, 0x00, 0xff };
static const uint8_t frametapBackgroundColor[FRAMETAP_BYTES_PER_PIXEL] = { 0x00, 0x00, 0x00, 0xff };

#define FRAMETAP_STRING(x)          #x
#define FRAMETAP_EXPAND_STRING(x)   FRAMETAP_STRING(x)

/* The quad comes from gl_VertexID alone, so the program reads no vertex attribute and the guest's arrays stay out of it. */
static const char frametapVertexShader[] =
    "#version 130\n"
    "uniform vec4 target_rect;\n"
    "out vec2 line_position;\n"
    "void main() {\n"
    "    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
    "    gl_Position = vec4(mix(target_rect.xy, target_rect.zw, corner), 0.0, 1.0);\n"
    "    line_position = corner;\n"
    "}\n";

/* texelFetch reads the atlas without any sampler state, so filtering and wrap modes on that unit do not matter. */
static const char frametapFragmentShader[] =
    "#version 130\n"
    "#define GLYPH_WIDTH " FRAMETAP_EXPAND_STRING(FRAMETAP_GLYPH_WIDTH) "\n"
    "#define GLYPH_HEIGHT " FRAMETAP_EXPAND_STRING(FRAMETAP_GLYPH_HEIGHT) "\n"
    "#define LINE_GLYPHS " FRAMETAP_EXPAND_STRING(FRAMETAP_LINE_GLYPHS) "\n"
    "uniform sampler2D glyph_atlas;\n"
    "uniform int line_glyphs[LINE_GLYPHS];\n"
    "uniform int line_length;\n"
    "in vec2 line_position;\n"
    "out vec4 fragment_color;\n"
    "void main() {\n"
    "    int column = int(line_position.x * float(line_length * GLYPH_WIDTH));\n"
    "    int row = min(int(line_position.y * float(GLYPH_HEIGHT)), GLYPH_HEIGHT - 1);\n"
    "    int slot = min(column / GLYPH_WIDTH, line_length - 1);\n"
    "    int atlas_x = line_glyphs[slot] * GLYPH_WIDTH + (column - slot * GLYPH_WIDTH);\n"
    "    fragment_color = texelFetch(glyph_atlas, ivec2(atlas_x, row), 0);\n"
    "}\n";

/* Switched off while the quad draws and switched on again afterwards, in every profile. */
static const GLenum frametapCapabilities[] = {
    GL_BLEND, GL_DEPTH_TEST, GL_STENCIL_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_COLOR_LOGIC_OP, GL_RASTERIZER_DISCARD, GL_FRAMEBUFFER_SRGB, GL_POLYGON_SMOOTH,
    GL_CLIP_DISTANCE0, GL_CLIP_DISTANCE1, GL_CLIP_DISTANCE2, GL_CLIP_DISTANCE3, GL_CLIP_DISTANCE4, GL_CLIP_DISTANCE5,
};
#define FRAMETAP_CAPABILITY_COUNT ((int)ARRAY_SIZE(frametapCapabilities))

/* The same, but only a compatibility context knows them -- asking a core context would leave GL_INVALID_ENUM in the guest's error state. */
#define GL_ALPHA_TEST_COMPAT        0x0BC0
#define GL_POLYGON_STIPPLE_COMPAT   0x0B42
static const GLenum frametapCompatibilityCapabilities[] = { GL_ALPHA_TEST_COMPAT, GL_POLYGON_STIPPLE_COMPAT };
#define FRAMETAP_COMPATIBILITY_CAPABILITY_COUNT ((int)ARRAY_SIZE(frametapCompatibilityCapabilities))

typedef enum {
    FRAMETAP_CONTEXT_UNUSED = 0,
    FRAMETAP_CONTEXT_READY = 1,
    /* The program did not build. The frames are still counted, only nothing is drawn. */
    FRAMETAP_CONTEXT_UNUSABLE = 2,
} FrametapContextState;

typedef struct {
    const void *key;
    FrametapContextState state;
    int core_profile;
    GLuint atlas_texture;
    GLuint program;
    GLuint vertex_array;
    GLint target_rect_location;
    GLint line_glyphs_location;
    GLint line_length_location;
    uint32_t line_serial;
    int target_drawable_width;
    int target_drawable_height;
} FrametapContext;

static struct {
    int level;
    FrametapPage *page;
    uint32_t page_sequence;
    uint64_t frame_count;
    int64_t last_frame_ns;
    FrametapSource last_source;
    int64_t last_swap_ns;
    int window_blit_pending;
    int64_t interval_start_ns;
    uint32_t interval_frames;
    uint64_t interval_cost_ticks;
    uint32_t slot_frames[FRAMETAP_WINDOW_SLOTS];
    int64_t slot_duration_ns[FRAMETAP_WINDOW_SLOTS];
    int next_slot;
    int64_t calibration_ns;
    int64_t calibration_ticks;
    uint32_t frames_per_second_x100;
    uint32_t overlay_cost_ns;
    GLint line_glyphs[FRAMETAP_LINE_GLYPHS];
    int line_length;
    uint32_t line_serial;
    FrametapContext contexts[FRAMETAP_MAX_CONTEXTS];
} frametap;

void frametap_init(void *page)
{
    const char *setting = getenv("QEMU_3DFX_FRAMETAP");
    const int requested_level = (setting)? atoi(setting):FRAMETAP_LEVEL_OFF;
    const int bounded_level = MIN(requested_level, FRAMETAP_LEVEL_RATE_AND_COST);
    frametap.level = MAX(bounded_level, FRAMETAP_LEVEL_OFF);

    frametap.page = page;
    frametap.page->magic = FRAMETAP_PAGE_MAGIC;
    frametap.page->version = FRAMETAP_PAGE_VERSION;
    frametap.page->size = sizeof(FrametapPage);
    frametap.page->level = frametap.level;

    if (frametap.level != FRAMETAP_LEVEL_OFF)
        fprintf(stderr, "qemu-3dfx frametap: level %d, page at 0x%08x\n", frametap.level, FRAMETAP_PAGE_BASE);
}

static void frametap_format_line(const double frames_per_second, const double cost_ns)
{
    const unsigned rounded_rate = (unsigned)(frames_per_second + FRAMETAP_ROUND_TO_NEAREST);
    char text[FRAMETAP_LINE_GLYPHS + 1];
    int length;
    /* Fixed widths, so the box keeps its size while the numbers change. */
    if (frametap.level == FRAMETAP_LEVEL_RATE_AND_COST) {
        const double cost_us = cost_ns / FRAMETAP_NANOSECONDS_PER_MICROSECOND;
        length = snprintf(text, sizeof(text), " %5u FPS %6.1f us ", rounded_rate, cost_us);
    }
    else
        length = snprintf(text, sizeof(text), " %5u FPS ", rounded_rate);
    frametap.line_length = MIN(length, FRAMETAP_LINE_GLYPHS);

    for (int i = 0; i < frametap.line_length; i++) {
        const int character = (unsigned char)text[i];
        const int in_atlas = (character >= FRAMETAP_FIRST_GLYPH) && (character < FRAMETAP_FIRST_GLYPH + FRAMETAP_GLYPH_COUNT);
        const int shown_character = (in_atlas)? character:FRAMETAP_UNKNOWN_GLYPH;
        frametap.line_glyphs[i] = shown_character - FRAMETAP_FIRST_GLYPH;
    }
    frametap.line_serial++;
}

static void frametap_close_interval(const int64_t now_ns, const int64_t now_ticks, const int64_t interval_ns)
{
    const int slot = frametap.next_slot;
    frametap.slot_frames[slot] = frametap.interval_frames;
    frametap.slot_duration_ns[slot] = interval_ns;
    frametap.next_slot = (slot + 1) % FRAMETAP_WINDOW_SLOTS;

    /* The rate covers the last second, four slots of a quarter each, like the one number per second Fraps shows. */
    uint64_t window_frames = 0;
    int64_t window_ns = 0;
    for (int i = 0; i < FRAMETAP_WINDOW_SLOTS; i++) {
        window_frames += frametap.slot_frames[i];
        window_ns += frametap.slot_duration_ns[i];
    }
    const double frames_per_second = (double)window_frames * NANOSECONDS_PER_SECOND / window_ns;

    /* rdtsc ticks against the host clock since the first frame, so the cost needs no calibration of its own.
     * The cost of a frame is added after its count, so every interval carries the cost of exactly as many frames as it counts, shifted by one.
     */
    const int64_t calibration_ticks = now_ticks - frametap.calibration_ticks;
    const int64_t calibration_ns = now_ns - frametap.calibration_ns;
    const double nanoseconds_per_tick = (calibration_ticks > 0)? ((double)calibration_ns / calibration_ticks):0;
    const double interval_cost_ns = nanoseconds_per_tick * frametap.interval_cost_ticks;
    const double cost_ns = interval_cost_ns / frametap.interval_frames;

    frametap.frames_per_second_x100 = (uint32_t)(frames_per_second * FRAMETAP_RATE_TO_HUNDREDTHS + FRAMETAP_ROUND_TO_NEAREST);
    frametap.overlay_cost_ns = (uint32_t)(cost_ns + FRAMETAP_ROUND_TO_NEAREST);
    frametap_format_line(frames_per_second, cost_ns);

    frametap.interval_start_ns = now_ns;
    frametap.interval_frames = 0;
    frametap.interval_cost_ticks = 0;
}

static void frametap_count_frame(const int64_t now_ns, const int64_t now_ticks)
{
    if (!frametap.interval_start_ns) {
        frametap.interval_start_ns = now_ns;
        frametap.calibration_ns = now_ns;
        frametap.calibration_ticks = now_ticks;
    }
    frametap.frame_count++;
    frametap.interval_frames++;

    const int64_t interval_ns = now_ns - frametap.interval_start_ns;
    if (interval_ns >= FRAMETAP_UPDATE_INTERVAL_NS)
        frametap_close_interval(now_ns, now_ticks, interval_ns);
}

static uint8_t *frametap_build_atlas_pixels(void)
{
    const SFG_Font *font = &fgFontFixed8x13;
    const int atlas_width = FRAMETAP_GLYPH_COUNT * FRAMETAP_GLYPH_WIDTH;
    const int row_bytes = atlas_width * FRAMETAP_BYTES_PER_PIXEL;
    const int glyph_rows = MIN(font->Height, FRAMETAP_GLYPH_HEIGHT);
    uint8_t *pixels = g_malloc0(row_bytes * FRAMETAP_GLYPH_HEIGHT);

    /* fgfont.h stores each glyph the way glBitmap takes it: a width byte, then one byte per row from the bottom up, the leftmost pixel in the top bit.
     * GL textures run from the bottom up as well, so row r of the glyph is row r of the texture.
     */
    for (int glyph = 0; glyph < FRAMETAP_GLYPH_COUNT; glyph++) {
        const unsigned char *face = font->Characters[FRAMETAP_FIRST_GLYPH + glyph];
        for (int row = 0; row < FRAMETAP_GLYPH_HEIGHT; row++) {
            const int row_bits = (row < glyph_rows)? face[FGFONT_FIRST_ROW_OFFSET + row]:0;
            for (int column = 0; column < FRAMETAP_GLYPH_WIDTH; column++) {
                const int bit_index = (FRAMETAP_GLYPH_WIDTH - 1) - column;
                const int pixel_set = (row_bits >> bit_index) & 1;
                const uint8_t *color = (pixel_set)? frametapTextColor:frametapBackgroundColor;
                const int pixel_x = glyph * FRAMETAP_GLYPH_WIDTH + column;
                uint8_t *pixel = pixels + row * row_bytes + pixel_x * FRAMETAP_BYTES_PER_PIXEL;
                memcpy(pixel, color, FRAMETAP_BYTES_PER_PIXEL);
            }
        }
    }
    return pixels;
}

static int frametap_context_is_core(void)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);
    MESA_PFN(PFNGLGETSTRINGPROC,   glGetString);

    /* GL_CONTEXT_PROFILE_MASK is only a valid query from 3.2 on, and an invalid one would stay in the guest's error state. */
    const GLubyte *version_string = PFN_CALL(glGetString(GL_VERSION));
    int major = 0, minor = 0;
    if (version_string)
        sscanf((const char *)version_string, "%d.%d", &major, &minor);
    const int below_core_version = (major < FRAMETAP_CORE_PROFILE_MIN_MAJOR) || ((major == FRAMETAP_CORE_PROFILE_MIN_MAJOR) && (minor < FRAMETAP_CORE_PROFILE_MIN_MINOR));
    if (below_core_version)
        return 0;

    GLint profile_mask = 0;
    PFN_CALL(glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &profile_mask));
    return (profile_mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0;
}

static GLuint frametap_compile_shader(const GLenum kind, const char *source, int *compiled)
{
    MESA_PFN(PFNGLCOMPILESHADERPROC, glCompileShader);
    MESA_PFN(PFNGLCREATESHADERPROC,  glCreateShader);
    MESA_PFN(PFNGLGETSHADERIVPROC,   glGetShaderiv);
    MESA_PFN(PFNGLSHADERSOURCEPROC,  glShaderSource);

    const GLuint shader = PFN_CALL(glCreateShader(kind));
    PFN_CALL(glShaderSource(shader, 1, &source, NULL));
    PFN_CALL(glCompileShader(shader));
    GLint status = GL_FALSE;
    PFN_CALL(glGetShaderiv(shader, GL_COMPILE_STATUS, &status));
    *compiled = (status == GL_TRUE);
    return shader;
}

/* Builds the program and looks up its uniforms. Needs no binding: the uniforms are set with glProgramUniform later. */
static int frametap_build_program(FrametapContext *context, const GLint atlas_unit)
{
    MESA_PFN(PFNGLATTACHSHADERPROC,          glAttachShader);
    MESA_PFN(PFNGLBINDFRAGDATALOCATIONPROC,  glBindFragDataLocation);
    MESA_PFN(PFNGLCREATEPROGRAMPROC,         glCreateProgram);
    MESA_PFN(PFNGLDELETESHADERPROC,          glDeleteShader);
    MESA_PFN(PFNGLGETPROGRAMINFOLOGPROC,     glGetProgramInfoLog);
    MESA_PFN(PFNGLGETPROGRAMIVPROC,          glGetProgramiv);
    MESA_PFN(PFNGLGETUNIFORMLOCATIONPROC,    glGetUniformLocation);
    MESA_PFN(PFNGLLINKPROGRAMPROC,           glLinkProgram);
    MESA_PFN(PFNGLPROGRAMUNIFORM1IVPROC,     glProgramUniform1iv);

    int vertex_compiled = 0, fragment_compiled = 0;
    const GLuint vertex_shader = frametap_compile_shader(GL_VERTEX_SHADER, frametapVertexShader, &vertex_compiled);
    const GLuint fragment_shader = frametap_compile_shader(GL_FRAGMENT_SHADER, frametapFragmentShader, &fragment_compiled);
    context->program = PFN_CALL(glCreateProgram());
    PFN_CALL(glAttachShader(context->program, vertex_shader));
    PFN_CALL(glAttachShader(context->program, fragment_shader));
    PFN_CALL(glBindFragDataLocation(context->program, 0, "fragment_color"));
    PFN_CALL(glLinkProgram(context->program));
    /* Attached shaders live on with the program; deleting them now lets them go along with it. */
    PFN_CALL(glDeleteShader(vertex_shader));
    PFN_CALL(glDeleteShader(fragment_shader));

    GLint link_status = GL_FALSE;
    PFN_CALL(glGetProgramiv(context->program, GL_LINK_STATUS, &link_status));
    const int built = vertex_compiled && fragment_compiled && (link_status == GL_TRUE);
    if (!built) {
        char info_log[FRAMETAP_INFO_LOG_SIZE] = { 0 };
        PFN_CALL(glGetProgramInfoLog(context->program, sizeof(info_log) - 1, NULL, info_log));
        fprintf(stderr, "qemu-3dfx frametap: program did not build (vertex %d, fragment %d, link %d) %s\n", vertex_compiled, fragment_compiled, link_status, info_log);
        return 0;
    }

    context->target_rect_location = PFN_CALL(glGetUniformLocation(context->program, "target_rect"));
    context->line_glyphs_location = PFN_CALL(glGetUniformLocation(context->program, "line_glyphs"));
    context->line_length_location = PFN_CALL(glGetUniformLocation(context->program, "line_length"));
    const GLint atlas_location = PFN_CALL(glGetUniformLocation(context->program, "glyph_atlas"));
    PFN_CALL(glProgramUniform1iv(context->program, atlas_location, 1, &atlas_unit));
    return 1;
}

/* Uploads the atlas onto the last texture unit and leaves it bound there for good -- no application of the time comes near that unit.
 * Everything else it binds or sets on the way is put back.
 */
static void frametap_upload_atlas(FrametapContext *context, const int context_index, const GLint atlas_unit)
{
    MESA_PFN(PFNGLACTIVETEXTUREPROC,    glActiveTexture);
    MESA_PFN(PFNGLBINDBUFFERPROC,       glBindBuffer);
    MESA_PFN(PFNGLBINDTEXTUREPROC,      glBindTexture);
    MESA_PFN(PFNGLGENTEXTURESPROC,      glGenTextures);
    MESA_PFN(PFNGLGETINTEGERVPROC,      glGetIntegerv);
    MESA_PFN(PFNGLPIXELSTOREIPROC,      glPixelStorei);
    MESA_PFN(PFNGLTEXIMAGE2DPROC,       glTexImage2D);
    MESA_PFN(PFNGLTEXPARAMETERIPROC,    glTexParameteri);

    static const GLenum unpackLayoutParameters[] = { GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS };
    const int unpackLayoutParameterCount = ARRAY_SIZE(unpackLayoutParameters);
    GLint saved_unpack_layout[ARRAY_SIZE(unpackLayoutParameters)];
    GLint saved_active_texture = GL_TEXTURE0, saved_unpack_buffer = 0;
    PFN_CALL(glGetIntegerv(GL_ACTIVE_TEXTURE, &saved_active_texture));
    PFN_CALL(glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &saved_unpack_buffer));
    for (int i = 0; i < unpackLayoutParameterCount; i++)
        PFN_CALL(glGetIntegerv(unpackLayoutParameters[i], &saved_unpack_layout[i]));

    /* WineD3D keeps a pixel unpack buffer bound. With it bound, the pixel pointer below would be an offset into the guest's buffer. */
    PFN_CALL(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0));
    for (int i = 0; i < unpackLayoutParameterCount; i++)
        PFN_CALL(glPixelStorei(unpackLayoutParameters[i], 0));

    if (context->core_profile)
        PFN_CALL(glGenTextures(1, &context->atlas_texture));
    else
        context->atlas_texture = FRAMETAP_TEXTURE_NAME_BASE + context_index;

    uint8_t *atlas_pixels = frametap_build_atlas_pixels();
    const int atlas_width = FRAMETAP_GLYPH_COUNT * FRAMETAP_GLYPH_WIDTH;
    PFN_CALL(glActiveTexture(GL_TEXTURE0 + atlas_unit));
    PFN_CALL(glBindTexture(GL_TEXTURE_2D, context->atlas_texture));
    PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST));
    PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0));
    PFN_CALL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, atlas_width, FRAMETAP_GLYPH_HEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas_pixels));
    g_free(atlas_pixels);

    PFN_CALL(glActiveTexture(saved_active_texture));
    for (int i = 0; i < unpackLayoutParameterCount; i++)
        PFN_CALL(glPixelStorei(unpackLayoutParameters[i], saved_unpack_layout[i]));
    PFN_CALL(glBindBuffer(GL_PIXEL_UNPACK_BUFFER, saved_unpack_buffer));
}

/* Runs once per context. */
static void frametap_create(FrametapContext *context, const int context_index)
{
    MESA_PFN(PFNGLGENVERTEXARRAYSPROC,  glGenVertexArrays);
    MESA_PFN(PFNGLGETINTEGERVPROC,      glGetIntegerv);

    GLint texture_units = 0;
    PFN_CALL(glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &texture_units));
    const GLint atlas_unit = texture_units - 1;

    context->core_profile = frametap_context_is_core();
    context->line_serial = 0;
    context->target_drawable_width = 0;
    context->target_drawable_height = 0;
    frametap_upload_atlas(context, context_index, atlas_unit);
    PFN_CALL(glGenVertexArrays(1, &context->vertex_array));
    const int built = frametap_build_program(context, atlas_unit);
    context->state = (built)? FRAMETAP_CONTEXT_READY:FRAMETAP_CONTEXT_UNUSABLE;

    const char *profile_name = (context->core_profile)? "core":"compatibility";
    const char *overlay_state = (built)? "overlay on":"overlay off";
    fprintf(stderr, "qemu-3dfx frametap: context %p %s profile, atlas on unit %d, program %u -> %s\n", context->key, profile_name, atlas_unit, context->program, overlay_state);
}

static FrametapContext *frametap_find_context(const void *key)
{
    if (!key)
        return NULL;

    FrametapContext *free_context = NULL;
    int free_index = 0;
    for (int i = 0; i < FRAMETAP_MAX_CONTEXTS; i++) {
        FrametapContext *context = &frametap.contexts[i];
        if ((context->state != FRAMETAP_CONTEXT_UNUSED) && (context->key == key))
            return context;
        if (!free_context && (context->state == FRAMETAP_CONTEXT_UNUSED)) {
            free_context = context;
            free_index = i;
        }
    }
    if (free_context) {
        free_context->key = key;
        frametap_create(free_context, free_index);
    }
    return free_context;
}

/* Only when the drawable or the text changed: the target rectangle in normalized device coordinates, and the glyph numbers. */
static void frametap_update_uniforms(FrametapContext *context, const int drawable_width, const int drawable_height)
{
    MESA_PFN(PFNGLPROGRAMUNIFORM1IVPROC, glProgramUniform1iv);
    MESA_PFN(PFNGLPROGRAMUNIFORM4FPROC,  glProgramUniform4f);

    const int drawable_changed = (context->target_drawable_width != drawable_width) || (context->target_drawable_height != drawable_height);
    const int text_changed = (context->line_serial != frametap.line_serial);
    if (drawable_changed || text_changed) {
        /* Top left, like Fraps, in whole multiples of the glyph size so the pixels stay sharp. */
        const int height_steps = drawable_height / FRAMETAP_DRAWABLE_HEIGHT_PER_SCALE_STEP;
        const int scale = MAX(1, height_steps);
        const int margin = FRAMETAP_MARGIN_PIXELS * scale;
        const int line_width = frametap.line_length * FRAMETAP_GLYPH_WIDTH * scale;
        const int line_height = FRAMETAP_GLYPH_HEIGHT * scale;
        const float left = 2.0f * margin / drawable_width - 1.0f;
        const float right = 2.0f * (margin + line_width) / drawable_width - 1.0f;
        const float top = 1.0f - 2.0f * margin / drawable_height;
        const float bottom = 1.0f - 2.0f * (margin + line_height) / drawable_height;
        PFN_CALL(glProgramUniform4f(context->program, context->target_rect_location, left, bottom, right, top));
        context->target_drawable_width = drawable_width;
        context->target_drawable_height = drawable_height;
    }
    if (text_changed) {
        PFN_CALL(glProgramUniform1iv(context->program, context->line_glyphs_location, frametap.line_length, frametap.line_glyphs));
        PFN_CALL(glProgramUniform1iv(context->program, context->line_length_location, 1, &frametap.line_length));
        context->line_serial = frametap.line_serial;
    }
}

static void frametap_draw(FrametapContext *context, const int drawable_width, const int drawable_height)
{
    MESA_PFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    MESA_PFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    MESA_PFN(PFNGLCOLORMASKPROC,       glColorMask);
    MESA_PFN(PFNGLDISABLEPROC,         glDisable);
    MESA_PFN(PFNGLDRAWARRAYSPROC,      glDrawArrays);
    MESA_PFN(PFNGLENABLEPROC,          glEnable);
    MESA_PFN(PFNGLGETBOOLEANVPROC,     glGetBooleanv);
    MESA_PFN(PFNGLGETINTEGERVPROC,     glGetIntegerv);
    MESA_PFN(PFNGLISENABLEDPROC,       glIsEnabled);
    MESA_PFN(PFNGLPOLYGONMODEPROC,     glPolygonMode);
    MESA_PFN(PFNGLUSEPROGRAMPROC,      glUseProgram);
    MESA_PFN(PFNGLVIEWPORTPROC,        glViewport);

    frametap_update_uniforms(context, drawable_width, drawable_height);

    GLint saved_program = 0, saved_vertex_array = 0, saved_draw_framebuffer = 0;
    GLint saved_viewport[FRAMETAP_VIEWPORT_VALUES];
    GLint saved_polygon_mode[FRAMETAP_POLYGON_MODE_VALUES] = { GL_FILL, GL_FILL };
    GLboolean saved_color_mask[FRAMETAP_COLOR_MASK_VALUES];
    GLboolean capability_was_enabled[FRAMETAP_CAPABILITY_COUNT];
    GLboolean compatibility_capability_was_enabled[FRAMETAP_COMPATIBILITY_CAPABILITY_COUNT] = { GL_FALSE };
    PFN_CALL(glGetIntegerv(GL_CURRENT_PROGRAM, &saved_program));
    PFN_CALL(glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &saved_vertex_array));
    PFN_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &saved_draw_framebuffer));
    PFN_CALL(glGetIntegerv(GL_VIEWPORT, saved_viewport));
    PFN_CALL(glGetIntegerv(GL_POLYGON_MODE, saved_polygon_mode));
    PFN_CALL(glGetBooleanv(GL_COLOR_WRITEMASK, saved_color_mask));
    for (int i = 0; i < FRAMETAP_CAPABILITY_COUNT; i++) {
        capability_was_enabled[i] = PFN_CALL(glIsEnabled(frametapCapabilities[i]));
        if (capability_was_enabled[i])
            PFN_CALL(glDisable(frametapCapabilities[i]));
    }
    if (!context->core_profile) {
        for (int i = 0; i < FRAMETAP_COMPATIBILITY_CAPABILITY_COUNT; i++) {
            compatibility_capability_was_enabled[i] = PFN_CALL(glIsEnabled(frametapCompatibilityCapabilities[i]));
            if (compatibility_capability_was_enabled[i])
                PFN_CALL(glDisable(frametapCompatibilityCapabilities[i]));
        }
    }
    /* A framebuffer switch is paid for in the next swap, so the binding is only touched when it is not the window already. */
    if (saved_draw_framebuffer)
        PFN_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
    const int polygon_mode_changed = (saved_polygon_mode[0] != GL_FILL) || (saved_polygon_mode[1] != GL_FILL);
    if (polygon_mode_changed)
        PFN_CALL(glPolygonMode(GL_FRONT_AND_BACK, GL_FILL));
    const int color_mask_changed = !saved_color_mask[0] || !saved_color_mask[1] || !saved_color_mask[2] || !saved_color_mask[3];
    if (color_mask_changed)
        PFN_CALL(glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE));

    PFN_CALL(glViewport(0, 0, drawable_width, drawable_height));
    PFN_CALL(glUseProgram(context->program));
    PFN_CALL(glBindVertexArray(context->vertex_array));
    PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, FRAMETAP_QUAD_VERTICES));

    PFN_CALL(glBindVertexArray(saved_vertex_array));
    PFN_CALL(glUseProgram(saved_program));
    PFN_CALL(glViewport(saved_viewport[0], saved_viewport[1], saved_viewport[2], saved_viewport[3]));
    if (color_mask_changed)
        PFN_CALL(glColorMask(saved_color_mask[0], saved_color_mask[1], saved_color_mask[2], saved_color_mask[3]));
    /* A core context only takes GL_FRONT_AND_BACK, so front and back are only restored apart where they can differ. */
    const int polygon_modes_differ = !context->core_profile && (saved_polygon_mode[0] != saved_polygon_mode[1]);
    if (polygon_mode_changed && !polygon_modes_differ)
        PFN_CALL(glPolygonMode(GL_FRONT_AND_BACK, saved_polygon_mode[0]));
    else if (polygon_mode_changed) {
        PFN_CALL(glPolygonMode(GL_FRONT, saved_polygon_mode[0]));
        PFN_CALL(glPolygonMode(GL_BACK, saved_polygon_mode[1]));
    }
    if (saved_draw_framebuffer)
        PFN_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, saved_draw_framebuffer));
    for (int i = 0; i < FRAMETAP_COMPATIBILITY_CAPABILITY_COUNT; i++) {
        if (compatibility_capability_was_enabled[i])
            PFN_CALL(glEnable(frametapCompatibilityCapabilities[i]));
    }
    for (int i = 0; i < FRAMETAP_CAPABILITY_COUNT; i++) {
        if (capability_was_enabled[i])
            PFN_CALL(glEnable(frametapCapabilities[i]));
    }
}

static void frametap_publish(const int64_t now_ns, const int drawable_width, const int drawable_height)
{
    FrametapPage *page = frametap.page;
    const int64_t frame_time_ns = (frametap.last_frame_ns)? (now_ns - frametap.last_frame_ns):0;
    frametap.last_frame_ns = now_ns;

    /* The page is guest RAM, so the sequence is kept on the host side and only copied out. */
    frametap.page_sequence++;
    qatomic_set(&page->sequence, frametap.page_sequence);
    smp_wmb();
    page->level = frametap.level;
    page->frame_count = frametap.frame_count;
    page->last_frame_ns = now_ns;
    page->frame_time_ns = (uint32_t)MIN(frame_time_ns, (int64_t)UINT32_MAX);
    page->frames_per_second_x100 = frametap.frames_per_second_x100;
    page->overlay_cost_ns = frametap.overlay_cost_ns;
    page->drawable_width = drawable_width;
    page->drawable_height = drawable_height;
    page->frame_source = frametap.last_source;
    smp_wmb();
    frametap.page_sequence++;
    qatomic_set(&page->sequence, frametap.page_sequence);
}

/* One finished frame, whichever call ended it: count it, draw the overlay into it, publish the numbers. */
static void frametap_present(const void *context_key, const FrametapSource source, const int64_t now_ns, const int64_t work_start_ticks)
{
    frametap.last_source = source;
    frametap_count_frame(now_ns, work_start_ticks);

    int sizes[GUI_SIZE_COUNT] = { 0 };
    mesa_gui_fullscreen(sizes);
    const int drawable_width = sizes[GUI_SIZE_DRAWABLE_WIDTH];
    const int drawable_height = sizes[GUI_SIZE_DRAWABLE_HEIGHT];

    FrametapContext *context = frametap_find_context(context_key);
    const int can_draw = context && (context->state == FRAMETAP_CONTEXT_READY) && frametap.line_length && (drawable_width > 0) && (drawable_height > 0);
    if (can_draw)
        frametap_draw(context, drawable_width, drawable_height);

    frametap_publish(now_ns, drawable_width, drawable_height);

    const int64_t work_end_ticks = cpu_get_host_ticks();
    frametap.interval_cost_ticks += work_end_ticks - work_start_ticks;
}

void MesaFrametapSwap(const void *context_key)
{
    if (frametap.level == FRAMETAP_LEVEL_OFF)
        return;

    const int64_t work_start_ticks = cpu_get_host_ticks();
    const int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    frametap.last_swap_ns = now_ns;
    frametap.window_blit_pending = 0;
    frametap_present(context_key, FRAMETAP_SOURCE_SWAP, now_ns, work_start_ticks);
}

/* Runs before every guest glBlitFramebuffer. One that lands in the window ends the frame of a guest that never swaps -- Drakan draws into an FBO,
 * blits that into the window once per frame and flushes twice (docs/LOG.md [476], [489] in the project repository).
 */
void MesaFrametapWindowBlit(void)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);

    if (frametap.level == FRAMETAP_LEVEL_OFF)
        return;

    const int64_t work_start_ticks = cpu_get_host_ticks();
    GLint draw_framebuffer = 0;
    PFN_CALL(glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_framebuffer));
    if (!draw_framebuffer)
        frametap.window_blit_pending = 1;
    const int64_t work_end_ticks = cpu_get_host_ticks();
    frametap.interval_cost_ticks += work_end_ticks - work_start_ticks;
}

/* Runs before every guest glFlush and glFinish. The first one after a blit into the window presents that frame, unless the guest swaps. */
void MesaFrametapFlush(const void *context_key)
{
    if ((frametap.level == FRAMETAP_LEVEL_OFF) || !frametap.window_blit_pending)
        return;

    const int64_t work_start_ticks = cpu_get_host_ticks();
    const int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    frametap.window_blit_pending = 0;
    const int swapped_recently = frametap.last_swap_ns && ((now_ns - frametap.last_swap_ns) < FRAMETAP_SWAP_RECENT_NS);
    if (swapped_recently)
        return;
    frametap_present(context_key, FRAMETAP_SOURCE_FLUSH, now_ns, work_start_ticks);
}

/* A destroyed context takes its program, vertex array and texture along. The entry has to go before the next context can come back at the same address. */
void MesaFrametapForget(const void *context_key)
{
    for (int i = 0; i < FRAMETAP_MAX_CONTEXTS; i++) {
        FrametapContext *context = &frametap.contexts[i];
        if ((context->state != FRAMETAP_CONTEXT_UNUSED) && (context->key == context_key))
            memset(context, 0, sizeof(*context));
    }
}
