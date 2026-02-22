/* Minimal LVGL config for PRP recovery GUI (fbdev + touch) */
#pragma once

/* Color depth: most Android framebuffers on legacy devices are RGB565. */
#define LV_COLOR_DEPTH 16

/* Use the built-in malloc/free from libc (musl). */
#define LV_MEM_CUSTOM 0

#define LV_USE_LOG 0

/* Tick */
#define LV_TICK_CUSTOM 0

/* Input devices */
#define LV_USE_INDEV 1

/* Fonts: keep small to reduce binary size. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_DEFAULT &lv_font_montserrat_24

/* Themes/widgets used by the minimal UI */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_IMG 1
#define LV_USE_BAR 0
#define LV_USE_SLIDER 0
#define LV_USE_TEXTAREA 0

/* Explicitly disable widgets we don't need. LVGL defaults many to 1 if not set. */
#define LV_USE_KEYBOARD 0
#define LV_USE_SPINBOX 0

/* Allow filesystem driver (optional, can be useful later). */
#define LV_USE_FS_STDIO 1
