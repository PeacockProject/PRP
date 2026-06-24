/* Minimal LVGL config for PRP recovery GUI (fbdev + touch) */
#pragma once

/* Color depth: most Android framebuffers on legacy devices are RGB565. */
#define LV_COLOR_DEPTH 16

/* Use the built-in malloc/free from libc (musl). */
#define LV_MEM_CUSTOM 0
/* This is a phone/host recovery GUI, not an MCU: the default 48KB heap can't
 * hold full-screen gradients + a QR canvas + the on-screen keyboard at once,
 * which deadlocked the software renderer (gradient alloc fails -> the dirty
 * area never finishes -> the refresh timer busy-loops). Give it real room. */
#define LV_MEM_SIZE (8U * 1024U * 1024U)
/* Cache computed gradients so a full-screen bg gradient is reused across objects
 * and frames instead of re-allocated every draw. 0 = per-frame churn. */
#define LV_GRAD_CACHE_DEF_SIZE (256U * 1024U)

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
#define LV_USE_TILEVIEW 1
#define LV_USE_QRCODE 1
#define LV_USE_BAR 1
#define LV_USE_SLIDER 0
#define LV_USE_TEXTAREA 1
#define LV_USE_DROPDOWN 1
#define LV_USE_BTNMATRIX 1

/* Installer wizard on-screen input */
#define LV_USE_KEYBOARD 1
#define LV_USE_SPINBOX 0

/* Dark default theme so stock widgets (dropdown/textarea/keyboard) match the dark UI. */
#define LV_THEME_DEFAULT_DARK 1

/* Allow filesystem driver (optional, can be useful later). */
#define LV_USE_FS_STDIO 1
