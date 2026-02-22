// PRP logo loader using stb_image (PNG) and conversion to RGB565 for LVGL.

#include "prp_logo.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Keep stb small.
#define STBI_ONLY_PNG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

static uint16_t rgb8888_to_565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | ((uint16_t)(b >> 3) << 0));
}

static bool read_file_all(const char *path, uint8_t **out, size_t *out_sz) {
    *out = NULL;
    *out_sz = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;

    off_t end = lseek(fd, 0, SEEK_END);
    if(end <= 0) {
        close(fd);
        return false;
    }
    if(lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return false;
    }

    size_t sz = (size_t)end;
    uint8_t *buf = (uint8_t *)malloc(sz);
    if(!buf) {
        close(fd);
        return false;
    }

    size_t off = 0;
    while(off < sz) {
        ssize_t n = read(fd, buf + off, sz - off);
        if(n < 0) {
            if(errno == EINTR) continue;
            free(buf);
            close(fd);
            return false;
        }
        if(n == 0) break;
        off += (size_t)n;
    }
    close(fd);
    if(off != sz) {
        free(buf);
        return false;
    }

    *out = buf;
    *out_sz = sz;
    return true;
}

lv_img_dsc_t *prp_logo_load_png_rgb565(const char *path) {
    if(!path || !*path) return NULL;

    uint8_t *png = NULL;
    size_t png_sz = 0;
    if(!read_file_all(path, &png, &png_sz)) return NULL;

    int w = 0, h = 0, ch = 0;
    stbi_uc *rgba = stbi_load_from_memory((const stbi_uc *)png, (int)png_sz, &w, &h, &ch, 4);
    free(png);
    if(!rgba || w <= 0 || h <= 0) {
        if(rgba) stbi_image_free(rgba);
        return NULL;
    }

    // Hard limit to avoid allocating huge images by mistake.
    if(w > 1024 || h > 1024) {
        stbi_image_free(rgba);
        return NULL;
    }

    size_t px = (size_t)w * (size_t)h;
    uint16_t *rgb565 = (uint16_t *)malloc(px * sizeof(uint16_t));
    if(!rgb565) {
        stbi_image_free(rgba);
        return NULL;
    }

    // Composite on black (AppBar is black).
    for(size_t i = 0; i < px; i++) {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        uint8_t a = rgba[i * 4 + 3];
        // Premultiply by alpha onto black.
        r = (uint8_t)((uint32_t)r * (uint32_t)a / 255u);
        g = (uint8_t)((uint32_t)g * (uint32_t)a / 255u);
        b = (uint8_t)((uint32_t)b * (uint32_t)a / 255u);
        rgb565[i] = rgb8888_to_565(r, g, b);
    }
    stbi_image_free(rgba);

    lv_img_dsc_t *d = (lv_img_dsc_t *)malloc(sizeof(lv_img_dsc_t));
    if(!d) {
        free(rgb565);
        return NULL;
    }
    memset(d, 0, sizeof(*d));
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->data_size = (uint32_t)(px * 2u);
    d->data = (const uint8_t *)rgb565;
    return d;
}

void prp_logo_free(lv_img_dsc_t *dsc) {
    if(!dsc) return;
    free((void *)dsc->data);
    free(dsc);
}

lv_img_dsc_t *prp_logo_try_load(const char *const *paths) {
    if(!paths) return NULL;
    for(size_t i = 0; paths[i]; i++) {
        lv_img_dsc_t *d = prp_logo_load_png_rgb565(paths[i]);
        if(d) return d;
    }
    return NULL;
}

