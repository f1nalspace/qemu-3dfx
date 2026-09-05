/*
 * QEMU MESA GL Pass-Through
 *
 *  Copyright (c) ... in a Galaxy far, far away ...
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library;
 * if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"

#include "mesagl_impl.h"

int mesa_gui_fullscreen(const void *);

void MesaContextAttest(const char *div, int *out)
{
    const char *aiv[] = { ATTEST_IV };
    *out = 1;
    for (int i = 0; aiv[i]; i++) {
        *out = memcmp(div, aiv[i], strlen(aiv[i]))? 0:1;
        if (*out) break;
    }
}

/* Compatibility-profile calls, so glcorearb.h has neither the prototypes nor the bit. */
typedef void (APIENTRYP PFNGLPUSHCLIENTATTRIBCOMPATPROC)(GLbitfield mask);
typedef void (APIENTRYP PFNGLPOPCLIENTATTRIBCOMPATPROC)(void);
#define GL_CLIENT_VERTEX_ARRAY_BIT_COMPAT 0x00000002

static struct {
    unsigned vao, vbo;
    int prog, vert, frag, black;
    int adj, flip, has_swap;
} blit;
static unsigned blit_program_setup(void)
{
    MESA_PFN(PFNGLATTACHSHADERPROC,       glAttachShader);
    MESA_PFN(PFNGLBINDATTRIBLOCATIONPROC, glBindAttribLocation);
    MESA_PFN(PFNGLCOMPILESHADERPROC,      glCompileShader);
    MESA_PFN(PFNGLCREATEPROGRAMPROC,      glCreateProgram);
    MESA_PFN(PFNGLCREATESHADERPROC,       glCreateShader);
    MESA_PFN(PFNGLGETINTEGERVPROC,        glGetIntegerv);
    MESA_PFN(PFNGLGETSTRINGPROC,          glGetString);
    MESA_PFN(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation);
    MESA_PFN(PFNGLLINKPROGRAMPROC,        glLinkProgram);
    MESA_PFN(PFNGLSHADERSOURCEPROC,       glShaderSource);
    MESA_PFN(PFNGLUSEPROGRAMPROC,         glUseProgram);
    const char *vert_src[] = {
        "#version 120\n"
        "attribute vec2 in_position;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  texcoord = vec2(1 + in_position.x, 1 + in_position.y) * 0.5;\n"
        "  gl_Position = vec4(in_position, 0, 1);\n"
        "}\n",
        "#version 140\n"
        "#extension GL_ARB_explicit_attrib_location : require\n"
        "layout (location = 0) in vec2 in_position;\n"
        "out vec2 texcoord;\n"
        "void main() {\n"
        "  texcoord = vec2(1 + in_position.x, 1 + in_position.y) * 0.5;\n"
        "  gl_Position = vec4(in_position, 0, 1);\n"
        "}\n"
    };
    const char *frag_src[] = {
        "#version 120\n"
        "uniform sampler2D screen_texture;\n"
        "uniform bool frag_just_black;\n"
        "varying vec2 texcoord;\n"
        "void main() {\n"
        "  if (frag_just_black)\n"
        "    gl_FragColor = vec4(0,0,0,1);\n"
        "  else\n"
        "    gl_FragColor = texture2D(screen_texture, texcoord);\n"
        "}\n",
        "#version 140\n"
        "uniform sampler2D screen_texture;\n"
        "uniform bool frag_just_black;\n"
        "in vec2 texcoord;\n"
        "out vec4 fragColor;\n"
        "void main() {\n"
        "  if (frag_just_black)\n"
        "    fragColor = vec4(0,0,0,1);\n"
        "  else\n"
        "    fragColor = texture(screen_texture, texcoord);\n"
        "}\n"
    };
    int prog;
    if (!blit.prog) {
        int i = memcmp(PFN_CALL(glGetString(GL_VERSION)), "2.1 Metal",
                sizeof("2.1 Metal") - 1)? 1:0,
            srclen = ALIGNED((strlen(vert_src[i])+1));
        char *srcbuf = g_new0(char, srclen);
        const char *vert_buf[] = { srcbuf };
        strncpy(srcbuf, vert_src[i], srclen);
        if (blit.flip) {
            char *flip = strstr(srcbuf, "+ in_position.y");
            *flip = '-';
        }
        blit.vert = PFN_CALL(glCreateShader(GL_VERTEX_SHADER));
        PFN_CALL(glShaderSource(blit.vert, 1, vert_buf, 0));
        PFN_CALL(glCompileShader(blit.vert));
        g_free(srcbuf);
        blit.frag = PFN_CALL(glCreateShader(GL_FRAGMENT_SHADER));
        PFN_CALL(glShaderSource(blit.frag, 1, &frag_src[i], 0));
        PFN_CALL(glCompileShader(blit.frag));
        prog = PFN_CALL(glCreateProgram());
        PFN_CALL(glAttachShader(prog, blit.vert));
        PFN_CALL(glAttachShader(prog, blit.frag));
        if (!i)
            PFN_CALL(glBindAttribLocation(prog, 0, "in_position"));
        PFN_CALL(glLinkProgram(prog));
        blit.prog = prog;
    }
    PFN_CALL(glGetIntegerv(GL_CURRENT_PROGRAM, &prog));
    PFN_CALL(glUseProgram(blit.prog));
    blit.black = PFN_CALL(glGetUniformLocation(blit.prog, "frag_just_black"));
    return prog;
}
void MesaBlitFree(void)
{
    MESA_PFN(PFNGLDELETEBUFFERSPROC,      glDeleteBuffers);
    MESA_PFN(PFNGLDELETEPROGRAMPROC,      glDeleteProgram);
    MESA_PFN(PFNGLDELETESHADERPROC,       glDeleteShader);
    MESA_PFN(PFNGLDELETEVERTEXARRAYSPROC, glDeleteVertexArrays);
    if (blit.prog) {
        PFN_CALL(glDeleteProgram(blit.prog));
        PFN_CALL(glDeleteShader(blit.vert));
        PFN_CALL(glDeleteShader(blit.frag));
    }
    if (blit.vbo)
        PFN_CALL(glDeleteBuffers(1, &blit.vbo));
    if (blit.vao)
        PFN_CALL(glDeleteVertexArrays(1, &blit.vao));
    memset(&blit, 0, sizeof(blit));
}
struct save_states {
    int view[4];
    int draw_binding, read_binding, texture, texture_binding,
        vao_binding, vbo_binding, boolean_map;
};
#define FRAMEBUFFER_SRGB_(s) \
    (s.boolean_map & 2)
struct states_mapping {
    int gl_enum, *iv;
};
static const int boolean_states[] = {
    GL_FRAMEBUFFER_SRGB,
    GL_BLEND,
    GL_CULL_FACE,
    GL_DEPTH_TEST,
    GL_SCISSOR_TEST,
    GL_STENCIL_TEST,
    0,
};
static int blit_program_buffer(void *save_map, const int size, const void *data)
{
    MESA_PFN(PFNGLBINDBUFFERPROC,      glBindBuffer);
    MESA_PFN(PFNGLBINDVERTEXARRAYPROC, glBindVertexArray);
    MESA_PFN(PFNGLBUFFERDATAPROC,      glBufferData);
    MESA_PFN(PFNGLDISABLEPROC,         glDisable);
    MESA_PFN(PFNGLGENBUFFERSPROC,      glGenBuffers);
    MESA_PFN(PFNGLGENVERTEXARRAYSPROC, glGenVertexArrays);
    MESA_PFN(PFNGLGETINTEGERVPROC,     glGetIntegerv);
    MESA_PFN(PFNGLISENABLEDPROC,       glIsEnabled);
    MESA_PFN(PFNGLPUSHCLIENTATTRIBCOMPATPROC, glPushClientAttrib);

    struct save_states *last = (struct save_states *)save_map;

    struct states_mapping mapping[] = {
        { GL_VIEWPORT, last->view },
        { GL_FRAMEBUFFER_BINDING, &last->draw_binding },
        { GL_READ_FRAMEBUFFER_BINDING, &last->read_binding },
        { GL_ACTIVE_TEXTURE, &last->texture },
        { GL_TEXTURE_BINDING_2D, &last->texture_binding },
        { GL_VERTEX_ARRAY_BINDING, &last->vao_binding },
        { GL_ARRAY_BUFFER_BINDING, &last->vbo_binding },
        { GL_CONTEXT_PROFILE_MASK, &last->boolean_map },
        { 0, 0 },
    };
    for (int i = 0; mapping[i].gl_enum; i++)
        PFN_CALL(glGetIntegerv(mapping[i].gl_enum, mapping[i].iv));
    last->boolean_map &= GL_CONTEXT_CORE_PROFILE_BIT;

    for (int i = 0; boolean_states[i]; i++) {
        last->boolean_map |= PFN_CALL(glIsEnabled(boolean_states[i]))? (2 << i):0;
        if (last->boolean_map & (2 << i))
            PFN_CALL(glDisable(boolean_states[i]));
    }
    if (last->boolean_map & GL_CONTEXT_CORE_PROFILE_BIT) {
        if (!blit.vao)
            PFN_CALL(glGenVertexArrays(1, &blit.vao));
        PFN_CALL(glBindVertexArray(blit.vao));
    }
    else {
        /* Without a vertex array object the arrays are global state, and attribute 0 is
         * the one the blit shader draws from -- on NVIDIA it aliases the fixed-function
         * vertex pointer, which is what WineD3D draws its scene with. Overwriting it and
         * leaving it disabled stops the guest from drawing anything from the next frame
         * on: the scene disappears and never comes back. docs/LOG.md.
         */
        PFN_CALL(glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT_COMPAT));
    }
    if (!blit.vbo)
        PFN_CALL(glGenBuffers(1, &blit.vbo));
    PFN_CALL(glBindBuffer(GL_ARRAY_BUFFER, blit.vbo));
    PFN_CALL(glBufferData(GL_ARRAY_BUFFER, size, data, GL_STATIC_DRAW));
    return 0;
}
static void blit_restore_savemap(const void *save_map)
{
    MESA_PFN(PFNGLBINDBUFFERPROC,               glBindBuffer);
    MESA_PFN(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray);
    MESA_PFN(PFNGLENABLEPROC,                   glEnable);
    MESA_PFN(PFNGLPOPCLIENTATTRIBCOMPATPROC,    glPopClientAttrib);

    struct save_states *last = (struct save_states *)save_map;

    if (last->boolean_map & GL_CONTEXT_CORE_PROFILE_BIT)
        PFN_CALL(glBindVertexArray(last->vao_binding));
    else
        PFN_CALL(glPopClientAttrib());

    PFN_CALL(glBindBuffer(GL_ARRAY_BUFFER, last->vbo_binding));

    for (int i = 0; boolean_states[i]; i++) {
        if ((boolean_states[i] == GL_FRAMEBUFFER_SRGB)
                && !(last->read_binding == last->draw_binding))
            continue;
        if (last->boolean_map & (2 << i))
            PFN_CALL(glEnable(boolean_states[i]));
    }
}
/* qemu-3dfx: in full screen the upscaler is the only thing between a small guest image and
 * a large drawable, and when it fails there is nothing left to look at afterwards. This
 * writes down what it saw, switched on with QEMU_3DFX_UI_DIAG=1 -- the same knob as the
 * ui/sdl2.c diagnostics. It calls glGetError() and so eats the guest's pending error,
 * which is why it stays off by default. See docs/LOG.md.
 */
static int blit_diagnostics_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *value = getenv("QEMU_3DFX_UI_DIAG");
        enabled = (value && (*value != '0'))? 1:0;
    }
    return enabled;
}

enum {
    BLIT_PATH_IDLE = 0,
    BLIT_PATH_ADJUSTED,
    BLIT_PATH_COPY_TEXTURE,
    BLIT_PATH_FRAMEBUFFER,
};

static const char *blit_path_name(const int path)
{
    switch (path) {
        case BLIT_PATH_ADJUSTED:     return "uebersprungen";
        case BLIT_PATH_COPY_TEXTURE: return "textur";
        case BLIT_PATH_FRAMEBUFFER:  return "blit";
        default:                     return "aus";
    }
}

/* Where the scene actually is. A guest that presents from an FBO leaves its clear colour
 * in the default framebuffer, and a scaler that reads the wrong one shows a picture with
 * no scene in it -- the failure looks exactly like a broken scaler. One pixel from the
 * middle of the guest image, out of each candidate, settles it.
 */
static unsigned blit_probe_pixel(const unsigned framebuffer, const int x, const int y)
{
    MESA_PFN(PFNGLBINDFRAMEBUFFERPROC, glBindFramebuffer);
    MESA_PFN(PFNGLGETINTEGERVPROC,     glGetIntegerv);
    MESA_PFN(PFNGLREADPIXELSPROC,      glReadPixels);

    unsigned char pixel[4] = { 0, 0, 0, 0 };
    int read_binding = 0;

    PFN_CALL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_binding));
    PFN_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer));
    PFN_CALL(glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel));
    PFN_CALL(glBindFramebuffer(GL_READ_FRAMEBUFFER, read_binding));
    return (pixel[0] << 16) | (pixel[1] << 8) | pixel[2];
}

/* Sampled before anything is drawn -- afterwards every candidate carries our own output. */
static unsigned probe_default_framebuffer, probe_guest_framebuffer;

static void blit_probe_before_scaling(const int *v)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);

    int read_binding = 0;
    const int guest_width = v[0], guest_height = v[1] & 0x7FFFU;

    if (!blit_diagnostics_enabled() || !guest_width || !guest_height)
        return;
    PFN_CALL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_binding));
    probe_default_framebuffer = blit_probe_pixel(0, guest_width / 2, guest_height / 2);
    probe_guest_framebuffer = (read_binding)?
        blit_probe_pixel(read_binding, guest_width / 2, guest_height / 2):probe_default_framebuffer;
}

static void blit_diag(const int path, const int fullscreen, const int *v,
                      const int drawable_context)
{
    MESA_PFN(PFNGLGETERRORPROC,    glGetError);
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);

    static char last_line[256];
    char line[256];
    int view[4] = { 0, 0, 0, 0 }, read_binding = 0, draw_binding = 0, sample_buffers = 0;
    int read_buffer = 0, draw_buffer = 0;
    unsigned gl_error, pixel_default = 0, pixel_guest_fbo = 0;

    if (!blit_diagnostics_enabled())
        return;

    gl_error = PFN_CALL(glGetError());
    PFN_CALL(glGetIntegerv(GL_VIEWPORT, view));
    PFN_CALL(glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_binding));
    PFN_CALL(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &draw_binding));
    PFN_CALL(glGetIntegerv(GL_SAMPLE_BUFFERS, &sample_buffers));
    PFN_CALL(glGetIntegerv(GL_READ_BUFFER, &read_buffer));
    PFN_CALL(glGetIntegerv(GL_DRAW_BUFFER, &draw_buffer));
    pixel_default = probe_default_framebuffer;
    pixel_guest_fbo = probe_guest_framebuffer;

    snprintf(line, sizeof(line),
        "qemu-3dfx blit: %-13s vollbild=%d gast=%dx%d flaeche=%dx%d kontext=%d scaleroff=%d "
        "sichtfeld=%d,%d %dx%d lesen=%d(0x%04x) zeichnen=%d(0x%04x) proben=%d "
        "punkt0=%06x punktfbo=%06x fehler=0x%04x",
        blit_path_name(path), fullscreen, v[0], v[1] & 0x7FFFU, v[2], v[3],
        drawable_context, RenderScalerOff(),
        view[0], view[1], view[2], view[3],
        read_binding, read_buffer, draw_binding, draw_buffer, sample_buffers,
        pixel_default, pixel_guest_fbo, gl_error);
    if (!strcmp(line, last_line))
        return;
    strncpy(last_line, line, sizeof(last_line) - 1);
    fprintf(stderr, "%s\n", line);
}
void MesaBlitScale(void)
{
    MESA_PFN(PFNGLACTIVETEXTUREPROC,            glActiveTexture);
    MESA_PFN(PFNGLBINDTEXTUREPROC,              glBindTexture);
    MESA_PFN(PFNGLBLITFRAMEBUFFERPROC,          glBlitFramebuffer);
    MESA_PFN(PFNGLCOPYTEXIMAGE2DPROC,           glCopyTexImage2D);
    MESA_PFN(PFNGLDELETETEXTURESPROC,           glDeleteTextures);
    MESA_PFN(PFNGLDISABLEVERTEXATTRIBARRAYPROC, glDisableVertexAttribArray);
    MESA_PFN(PFNGLDRAWARRAYSPROC,               glDrawArrays);
    MESA_PFN(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray);
    MESA_PFN(PFNGLGENTEXTURESPROC,              glGenTextures);
    MESA_PFN(PFNGLTEXPARAMETERIPROC,            glTexParameteri);
    MESA_PFN(PFNGLUNIFORM1IPROC,                glUniform1i);
    MESA_PFN(PFNGLUSEPROGRAMPROC,               glUseProgram);
    MESA_PFN(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer);
    MESA_PFN(PFNGLVIEWPORTPROC,                 glViewport);
    MESA_PFN(PFNGLBINDFRAMEBUFFERPROC,          glBindFramebuffer);

    int v[4], fullscreen = mesa_gui_fullscreen(v), drawable_context, path = BLIT_PATH_IDLE;
    blit.has_swap = 1;

    if (blit.adj) {
        blit.adj = !blit.adj;
        blit_diag(BLIT_PATH_ADJUSTED, fullscreen, v, 1);
        return;
    }
    blit.flip = ScalerBlitFlip();
    drawable_context = DrawableContext();
    blit_probe_before_scaling(v);

    const int guest_width = v[0], guest_height = v[1] & 0x7FFFU;
    const int drawable_width = v[2], drawable_height = v[3];
    const int keep_aspect = (v[1] & (1 << 15))? 0:1;
    const int size_differs = (drawable_width != guest_width) || (drawable_height != guest_height);

    if (drawable_context && guest_width && guest_height && size_differs
            && (!fullscreen || RenderScalerOff())) {
        unsigned screen_texture, last_prog = blit_program_setup();
        int target_width = drawable_width, target_height = drawable_height;

        if (keep_aspect) {
            const float scale_to_width = (1.f * drawable_width) / guest_width;
            const float scale_to_height = (1.f * drawable_height) / guest_height;
            const float scale = (scale_to_width < scale_to_height)? scale_to_width:scale_to_height;
            target_width = guest_width * scale;
            target_height = guest_height * scale;
        }
        const int target_x = (drawable_width - target_width) / 2;
        const int target_y = (drawable_height - target_height) / 2;
        /* One quad, drawn twice: once black over the whole drawable for the letterbox
         * bars, once textured into the centred target rectangle.
         */
        const float coord[] = {
            -1,-1,  1,-1,  -1,1,  1,1,
        };

        struct save_states save_map;

        if (!blit_program_buffer(&save_map, sizeof(coord), coord)) {
            /* The image reaches the screen through the default framebuffer -- that is what
             * glXSwapBuffers presents -- so this always draws there, whatever the guest
             * left bound. Where it *reads* from depends: a guest that presents from an FBO
             * and leaves it bound (WineD3D does) has nothing but its clear colour in the
             * default framebuffer, and copying that gives a picture without the scene.
             */
            if (save_map.draw_binding)
                PFN_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0));
            PFN_CALL(glEnableVertexAttribArray(0));
            PFN_CALL(glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0));
            if (target_x || target_y) {
                PFN_CALL(glUniform1i(blit.black, GL_TRUE));
                PFN_CALL(glViewport(0,0,  drawable_width, drawable_height));
                PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)); /* letterbox */
            }
            if (save_map.read_binding) {
                /* Colour alone, and GL_LINEAR: a scaling blit that carries depth or
                 * stencil is an error, and an errored blit leaves the screen black.
                 * The guest's FBO is bottom-up against the window, hence the flipped
                 * destination.
                 */
                path = BLIT_PATH_FRAMEBUFFER;
                PFN_CALL(glBlitFramebuffer(0,0, guest_width,guest_height,
                    target_x, target_y + target_height, target_x + target_width, target_y,
                    GL_COLOR_BUFFER_BIT, GL_LINEAR));
            }
            else {
                path = BLIT_PATH_COPY_TEXTURE;
                PFN_CALL(glActiveTexture(GL_TEXTURE0));
                PFN_CALL(glGenTextures(1, &screen_texture));
                PFN_CALL(glBindTexture(GL_TEXTURE_2D, screen_texture));
                PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
                PFN_CALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
                PFN_CALL(glCopyTexImage2D(GL_TEXTURE_2D, 0, (FRAMEBUFFER_SRGB_(save_map) && ScalerSRGBCorr())?
                            GL_SRGB:GL_RGBA, 0,0, guest_width,guest_height, 0));
                PFN_CALL(glUniform1i(blit.black, GL_FALSE));
                PFN_CALL(glViewport(target_x,target_y,  target_width,target_height));
                PFN_CALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4)); /* scale */
                PFN_CALL(glDeleteTextures(1, &screen_texture));
                PFN_CALL(glActiveTexture(save_map.texture));
                PFN_CALL(glBindTexture(GL_TEXTURE_2D, save_map.texture_binding));
            }
            PFN_CALL(glDisableVertexAttribArray(0));
            PFN_CALL(glViewport(save_map.view[0], save_map.view[1],
                                save_map.view[2], save_map.view[3]));
            if (save_map.draw_binding)
                PFN_CALL(glBindFramebuffer(GL_DRAW_FRAMEBUFFER, save_map.draw_binding));
            blit_restore_savemap(&save_map);
        }
        PFN_CALL(glUseProgram(last_prog));
    }
    blit_diag(path, fullscreen, v, drawable_context);
}

void MesaRenderScaler(const uint32_t FEnum, void *args)
{
    MESA_PFN(PFNGLGETINTEGERVPROC, glGetIntegerv);
    int v[4], fullscreen = mesa_gui_fullscreen(v), framebuffer_binding, blit_adj = 0;
    uint32_t *box;

    PFN_CALL(glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer_binding));

    switch(FEnum) {
        case FEnum_glBlitFramebuffer:
        case FEnum_glBlitFramebufferEXT:
            box = &((uint32_t *)args)[4];
            blit_adj = 1;
            break;
        case FEnum_glScissor:
        case FEnum_glViewport:
            box = args;
            break;
        case GL_VIEWPORT:
            box = args;
            if (!box[0] && !box[1] && (v[3] > (v[1] & 0x7FFFU))) {
                box[2] = v[0];
                box[3] = v[1] & 0x7FFFU;
            }
            /* fall through */
        default:
            return;
    }
    if (DrawableContext() && !framebuffer_binding
            && (v[3] > (v[1] & 0x7FFFU))
            && (fullscreen || !blit.has_swap)
            && !RenderScalerOff()) {
        int aspect = (v[1] & (1 << 15))? 0:1,
            offs_x = v[2] - ((v[0] * 1.f * v[3]) / (v[1] & 0x7FFFU));
        offs_x >>= 1;
        for (int i = 0; i < 4; i++)
            box[i] *= (1.f * v[3]) / (v[1] & 0x7FFFU);
        if (aspect) {
            box[0] += offs_x;
            box[2] += (blit_adj)? box[0]:0;
        }
        else {
            box[0] *= (1.f * v[2]) / box[2];
            box[2] = v[2];
        }
        blit.adj = blit_adj;
    }
}

