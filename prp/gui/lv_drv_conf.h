/* Minimal lv_drivers config for PRP */
#pragma once

/* Display */
#define USE_FBDEV 1
#define FBDEV_PATH "/dev/fb0"

/* Input */
#define USE_EVDEV 1
#define EVDEV_NAME "/dev/input/event0" /* PRP will override at runtime via evdev_set_file() */

/* Unused */
#define USE_DRM 0
#define USE_SDL 0

