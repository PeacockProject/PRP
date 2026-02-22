// PRP logo loader for LVGL.
// Loads a PNG from disk, decodes it, and converts to RGB565 (LV_COLOR_DEPTH=16).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl/lvgl.h"

// Load a PNG from `path` and return an LVGL image descriptor.
// The returned descriptor (and its backing pixel buffer) are heap allocated and must be freed.
lv_img_dsc_t *prp_logo_load_png_rgb565(const char *path);

void prp_logo_free(lv_img_dsc_t *dsc);

// Try a list of paths (NULL-terminated), return the first successfully loaded logo.
lv_img_dsc_t *prp_logo_try_load(const char *const *paths);

