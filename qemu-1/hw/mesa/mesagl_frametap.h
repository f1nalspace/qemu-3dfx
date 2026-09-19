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

#ifndef MESAGL_FRAMETAP_H
#define MESAGL_FRAMETAP_H

#include <stdint.h>

/* A frame rate overlay the guest pays nothing for.
 * Most frames a GL guest presents end in MGLSwapBuffers() on the host, whatever API the game used in the guest -- OpenGL directly, Direct3D through WineD3D.
 * frametap counts the frame there, draws the rate into it right before the swap, and publishes the numbers in one page of guest RAM,
 * so a tool in the guest can read them with plain memory reads instead of a VM exit.
 * A guest that never swaps -- Drakan blits its frame into the window and calls glFlush -- is counted at that blit and drawn into at the next glFlush.
 * DirectDraw through WineD3D neither swaps nor blits: it draws one quad into the window and flushes, so a draw with the window bound ends a frame at the next glFlush too.
 * A Glide frame is counted and drawn into from OpenGLide, through the hook it calls right before its swap (setConfigPresentHook).
 *
 * Switched on with QEMU_3DFX_FRAMETAP=1 (the rate) or =2 (the rate and frametap's own cost per frame). Off, the swap hook is one predictable compare.
 * Every caller runs on a vCPU thread under the BQL, so there is no lock.
 */

/* One page of RAM in the gap between the end of mglshm (MESA_FIFO_BASE + MGLSHM_SIZE) and the mesapt register page. */
#define FRAMETAP_PAGE_BASE      0xefffc000
#define FRAMETAP_PAGE_SIZE      0x1000
#define FRAMETAP_PAGE_MAGIC     0x50415446  /* "FTAP" in memory */
#define FRAMETAP_PAGE_VERSION   1

/* What ended the last frame. */
typedef enum {
    FRAMETAP_SOURCE_NONE = 0,
    FRAMETAP_SOURCE_SWAP = 1,
    /* glFlush after a blit or a draw into the window. */
    FRAMETAP_SOURCE_FLUSH = 2,
    FRAMETAP_SOURCE_GLIDE = 3,
} FrametapSource;

typedef enum {
    FRAMETAP_LEVEL_OFF = 0,
    FRAMETAP_LEVEL_RATE = 1,
    FRAMETAP_LEVEL_RATE_AND_COST = 2,
} FrametapLevel;

/* The layout a guest reads: fixed-size fields in natural alignment, no padding.
 * The host makes sequence odd before it writes and even again afterwards. A reader copies the page and starts over while sequence was odd or changed underneath it.
 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint32_t sequence;
    uint32_t level;
    uint64_t frame_count;
    uint64_t last_frame_ns;
    uint32_t frame_time_ns;
    uint32_t frames_per_second_x100;
    uint32_t overlay_cost_ns;
    uint16_t drawable_width;
    uint16_t drawable_height;
    uint32_t frame_source;
    uint32_t reserved;
} FrametapPage;

void frametap_init(void *page);
void MesaFrametapSwap(const void *context_key);
void MesaFrametapWindowBlit(void);
void MesaFrametapWindowDraw(void);
void MesaFrametapFlush(const void *context_key);
void MesaFrametapForget(const void *context_key);
int MesaFrametapEnabled(void);
void MesaFrametapGlideSwap(const void *context_key, const int drawable_width, const int drawable_height);

#endif /* MESAGL_FRAMETAP_H */
