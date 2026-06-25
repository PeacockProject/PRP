// Minimal fbdev backend for PRP's LVGL GUI.
// We keep LVGL at RGB565 (16bpp) for memory, and convert to the actual
// framebuffer format at flush time (often ARGB8888 on Android kernels).
//
// This avoids the lv_drivers fbdev backend's assumption that LVGL color depth
// matches the framebuffer bit depth, which can cause corrupt output.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

typedef struct {
    int fd;
    uint8_t *mem;
    uint32_t mem_len;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t line_length; // bytes per line

    // fb_var_screeninfo bitfields we need for packing.
    uint8_t roff, rlen;
    uint8_t goff, glen;
    uint8_t boff, blen;
    uint8_t aoff, alen;
    uint32_t xoffset;
    uint32_t yoffset;
} prp_fbdev_t;

bool prp_fbdev_init(prp_fbdev_t *fb, const char *path);
void prp_fbdev_deinit(prp_fbdev_t *fb);

// Fill the visible framebuffer region with a solid color (in RGB565).
// This is used to avoid inheriting whatever a previous boot stage left in fb0.
void prp_fbdev_clear(prp_fbdev_t *fb, uint16_t rgb565);

void prp_fbdev_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p);

// Set an integer upscale factor: LVGL renders at a logical resolution and the
// flush writes each logical pixel as a factor×factor block to the panel. 1 = none.
void prp_fbdev_set_scale(int factor);
