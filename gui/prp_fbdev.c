// PRP fbdev backend for LVGL.
// Supports LV_COLOR_DEPTH=16 (RGB565) and converts to common fb formats.

#include "prp_fbdev.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

static prp_fbdev_t g_fb;

static inline uint32_t scale_chan(uint32_t v, uint32_t in_bits, uint32_t out_bits) {
    if(in_bits == 0 || out_bits == 0) return 0;
    const uint32_t in_max = (1u << in_bits) - 1u;
    const uint32_t out_max = (1u << out_bits) - 1u;
    return (v * out_max + in_max / 2u) / in_max;
}

static inline uint32_t get_r565(uint16_t c) { return (c >> 11) & 0x1Fu; }
static inline uint32_t get_g565(uint16_t c) { return (c >> 5) & 0x3Fu; }
static inline uint32_t get_b565(uint16_t c) { return c & 0x1Fu; }

static inline uint32_t pack_pixel(const prp_fbdev_t *fb, uint16_t rgb565) {
    uint32_t r = scale_chan(get_r565(rgb565), 5, fb->rlen);
    uint32_t g = scale_chan(get_g565(rgb565), 6, fb->glen);
    uint32_t b = scale_chan(get_b565(rgb565), 5, fb->blen);
    /*
     * Many Android-era fbdev drivers expose a "transp" bitfield but it's either
     * ignored by the display engine or can be reported inconsistently. Setting
     * alpha to a non-zero value has caused visible color artifacts on some devices
     * (e.g. full-screen red tint). For PRP we prefer deterministic RGB output,
     * so we always write alpha as 0.
     */
    uint32_t a = 0;

    uint32_t out = 0;
    out |= (r & ((1u << fb->rlen) - 1u)) << fb->roff;
    out |= (g & ((1u << fb->glen) - 1u)) << fb->goff;
    out |= (b & ((1u << fb->blen) - 1u)) << fb->boff;
    if(fb->alen) out |= (a & ((1u << fb->alen) - 1u)) << fb->aoff;
    return out;
}

bool prp_fbdev_init(prp_fbdev_t *fb, const char *path) {
    if(!fb || !path) return false;
    memset(fb, 0, sizeof(*fb));

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if(fd < 0) {
        perror("prp_fbdev: open");
        return false;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if(ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("prp_fbdev: FBIOGET_FSCREENINFO");
        close(fd);
        return false;
    }
    if(ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("prp_fbdev: FBIOGET_VSCREENINFO");
        close(fd);
        return false;
    }

    uint32_t mem_len = (uint32_t)finfo.smem_len;
    uint8_t *mem = mmap(NULL, mem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if(mem == MAP_FAILED) {
        perror("prp_fbdev: mmap");
        close(fd);
        return false;
    }

    fb->fd = fd;
    fb->mem = mem;
    fb->mem_len = mem_len;
    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->bpp = vinfo.bits_per_pixel;
    fb->line_length = (uint32_t)finfo.line_length;
    fb->xoffset = vinfo.xoffset;
    fb->yoffset = vinfo.yoffset;

    fb->roff = vinfo.red.offset;
    fb->rlen = vinfo.red.length;
    fb->goff = vinfo.green.offset;
    fb->glen = vinfo.green.length;
    fb->boff = vinfo.blue.offset;
    fb->blen = vinfo.blue.length;
    fb->aoff = vinfo.transp.offset;
    fb->alen = vinfo.transp.length;

    // Keep a global for the LVGL flush callback.
    g_fb = *fb;

    // Debug: write fb format info for on-device troubleshooting.
    FILE *df = fopen("/tmp/prp-gui-fbinfo.txt", "w");
    if(df) {
        fprintf(df, "path=%s\n", path);
        fprintf(df, "xres=%u yres=%u xoff=%u yoff=%u bpp=%u line_len=%u smem_len=%u\n",
                vinfo.xres, vinfo.yres, vinfo.xoffset, vinfo.yoffset, vinfo.bits_per_pixel,
                (unsigned)finfo.line_length, (unsigned)finfo.smem_len);
        fprintf(df, "red off=%u len=%u\n", vinfo.red.offset, vinfo.red.length);
        fprintf(df, "green off=%u len=%u\n", vinfo.green.offset, vinfo.green.length);
        fprintf(df, "blue off=%u len=%u\n", vinfo.blue.offset, vinfo.blue.length);
        fprintf(df, "transp off=%u len=%u\n", vinfo.transp.offset, vinfo.transp.length);
        fclose(df);
    }

    return fb->width > 0 && fb->height > 0 && fb->line_length > 0;
}

void prp_fbdev_deinit(prp_fbdev_t *fb) {
    if(!fb) return;
    if(fb->mem && fb->mem_len) {
        munmap(fb->mem, fb->mem_len);
    }
    if(fb->fd > 0) close(fb->fd);
    memset(fb, 0, sizeof(*fb));
    memset(&g_fb, 0, sizeof(g_fb));
}

static inline uint8_t *row_ptr(const prp_fbdev_t *fb, int32_t x, int32_t y) {
    uint32_t bytespp = fb->bpp / 8u;
    uint32_t px_off = (uint32_t)(x + (int32_t)fb->xoffset);
    uint32_t py_off = (uint32_t)(y + (int32_t)fb->yoffset);
    uint32_t off = py_off * fb->line_length + px_off * bytespp;
    if(off >= fb->mem_len) return NULL;
    return fb->mem + off;
}

void prp_fbdev_clear(prp_fbdev_t *fb, uint16_t rgb565) {
    if(!fb || !fb->mem || fb->width == 0 || fb->height == 0 || fb->line_length == 0) return;
    const uint32_t bytespp = fb->bpp / 8u;
    if(bytespp == 0) return;

    const uint32_t px = fb->width;
    const uint32_t packed = pack_pixel(fb, rgb565);

    for(uint32_t y = 0; y < fb->height; y++) {
        uint8_t *dst = row_ptr(fb, 0, (int32_t)y);
        if(!dst) break;
        if(fb->bpp == 16) {
            uint16_t *d16 = (uint16_t *)dst;
            const uint16_t p16 = (uint16_t)packed;
            for(uint32_t x = 0; x < px; x++) d16[x] = p16;
        } else if(fb->bpp == 32) {
            uint32_t *d32 = (uint32_t *)dst;
            for(uint32_t x = 0; x < px; x++) d32[x] = packed;
        } else if(fb->bpp == 24) {
            const uint8_t b0 = (uint8_t)(packed & 0xFFu);
            const uint8_t b1 = (uint8_t)((packed >> 8) & 0xFFu);
            const uint8_t b2 = (uint8_t)((packed >> 16) & 0xFFu);
            for(uint32_t x = 0; x < px; x++) {
                dst[x * 3 + 0] = b0;
                dst[x * 3 + 1] = b1;
                dst[x * 3 + 2] = b2;
            }
        } else {
            // Unknown bpp: safest is a zero-fill of the active line region.
            memset(dst, 0, (size_t)px * (size_t)bytespp);
        }
    }
    // Make sure the display sees the new contents promptly.
    (void)msync(fb->mem, fb->mem_len, MS_SYNC);
}

// Integer upscale factor (1 = render 1:1). When >1, LVGL renders at a logical
// resolution and each logical pixel is written as a factor×factor block.
static int g_scale = 1;
void prp_fbdev_set_scale(int factor) { g_scale = (factor < 1) ? 1 : factor; }

void prp_fbdev_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
    const prp_fbdev_t *fb = &g_fb;
    if(!fb->mem || fb->width == 0 || fb->height == 0) {
        lv_disp_flush_ready(drv);
        return;
    }

    if(area->x2 < 0 || area->y2 < 0 || area->x1 > (int32_t)fb->width - 1 || area->y1 > (int32_t)fb->height - 1) {
        lv_disp_flush_ready(drv);
        return;
    }

    int32_t x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t x2 = area->x2 > (int32_t)fb->width - 1 ? (int32_t)fb->width - 1 : area->x2;
    int32_t y2 = area->y2 > (int32_t)fb->height - 1 ? (int32_t)fb->height - 1 : area->y2;

    const int32_t w = (x2 - x1 + 1);

    // Upscale path: LVGL rendered at a logical resolution; write each logical
    // pixel as a g_scale×g_scale block. area/color_p are in logical coords.
    if(g_scale > 1) {
        const int s = g_scale;
        const uint32_t bytespp = fb->bpp / 8u;
        for(int32_t ly = y1; ly <= y2; ly++) {
            for(int sy = 0; sy < s; sy++) {
                int32_t fy = ly * s + sy;
                if(fy >= (int32_t)fb->height) break;
                for(int32_t lx = 0; lx < w; lx++) {
                    const uint16_t c = color_p[lx].full;
                    const uint32_t packed = (fb->bpp == 16) ? c : pack_pixel(fb, c);
                    int32_t fx0 = (x1 + lx) * s;
                    for(int sx = 0; sx < s; sx++) {
                        int32_t fx = fx0 + sx;
                        if(fx >= (int32_t)fb->width) break;
                        uint8_t *dst = row_ptr(fb, fx, fy);
                        if(!dst) continue;
                        if(fb->bpp == 16) {
                            *(uint16_t *)dst = (uint16_t)packed;
                        } else if(fb->bpp == 32) {
                            *(uint32_t *)dst = packed;
                        } else if(fb->bpp == 24) {
                            dst[0] = (uint8_t)(packed & 0xFFu);
                            dst[1] = (uint8_t)((packed >> 8) & 0xFFu);
                            dst[2] = (uint8_t)((packed >> 16) & 0xFFu);
                        } else {
                            (void)bytespp;
                        }
                    }
                }
            }
            color_p += w;
        }
        lv_disp_flush_ready(drv);
        return;
    }

    // Fast path: framebuffer is RGB565 and LVGL is RGB565.
    if(fb->bpp == 16) {
        for(int32_t y = y1; y <= y2; y++) {
            uint8_t *dst = row_ptr(fb, x1, y);
            if(!dst) break;
            memcpy(dst, (const void *)color_p, (size_t)w * 2u);
            color_p += w;
        }
        lv_disp_flush_ready(drv);
        return;
    }

    // Common Android path: framebuffer is 32bpp (ARGB/XRGB), LVGL is RGB565.
    if(fb->bpp == 32) {
        for(int32_t y = y1; y <= y2; y++) {
            uint8_t *dst = row_ptr(fb, x1, y);
            if(!dst) break;
            uint32_t *dst32 = (uint32_t *)dst;
            for(int32_t x = 0; x < w; x++) {
                // lv_color_t at 16bpp is a packed 16-bit value.
                const uint16_t c = color_p[x].full;
                dst32[x] = pack_pixel(fb, c);
            }
            color_p += w;
        }
        lv_disp_flush_ready(drv);
        return;
    }

    // 24bpp: pack into 3 bytes per pixel in little-endian order.
    if(fb->bpp == 24) {
        for(int32_t y = y1; y <= y2; y++) {
            uint8_t *dst = row_ptr(fb, x1, y);
            if(!dst) break;
            for(int32_t x = 0; x < w; x++) {
                const uint16_t c = color_p[x].full;
                const uint32_t p = pack_pixel(fb, c);
                // This assumes packed 24bpp in the lower 24 bits.
                dst[x * 3 + 0] = (uint8_t)(p & 0xFFu);
                dst[x * 3 + 1] = (uint8_t)((p >> 8) & 0xFFu);
                dst[x * 3 + 2] = (uint8_t)((p >> 16) & 0xFFu);
            }
            color_p += w;
        }
        lv_disp_flush_ready(drv);
        return;
    }

    // Unsupported format: just mark flush complete.
    lv_disp_flush_ready(drv);
}
