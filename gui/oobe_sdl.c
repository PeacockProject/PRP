// OOBE host simulator: SDL2 backend that launches the first-boot OOBE wizard straight away, so we
// can iterate on the polymorphic blueprint UI on the desktop. Point it at a local blueprint dir
// (holding configure.toml) with --local <dir> (default: current dir). The apply step is mocked here
// (no /sbin/peacock-oobe present), so it walks the screens + a fake progress bar.

#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"
#include "lv_drivers/sdl/sdl.h"

#include "oobe_wizard.h"

static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

int main(int argc, char **argv) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    int scale = 100;
    const char *env = getenv("PRP_GUI_SCALE");
    if(env && *env) scale = atoi(env);
    const char *local = getenv("OOBE_BP_LOCAL");
    if(!local) local = ".";
    for(int i = 1; i < argc; i++) {
        if(!strcmp(argv[i], "--local") && i + 1 < argc) local = argv[++i];
        else if(!strcmp(argv[i], "--scale") && i + 1 < argc) scale = atoi(argv[++i]);
    }

    lv_init();
    sdl_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SDL_HOR_RES * 120];
    static lv_color_t buf2[SDL_HOR_RES * 120];
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, SDL_HOR_RES * 120);
    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_display_flush;
    disp_drv.hor_res = SDL_HOR_RES;
    disp_drv.ver_res = SDL_VER_RES;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);

    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    oobe_cfg_t cfg = {0};
    cfg.screen_w = SDL_HOR_RES;
    cfg.screen_h = SDL_VER_RES;
    cfg.scale_pct = scale;
    cfg.device_name = "QEMU x86_64";
    cfg.flavor = "Arch";
    cfg.root = "/tmp/oobe-root";
    cfg.blueprint_local = local; // sim: render from a local configure.toml, mock the apply
    prp_oobe_show(&cfg);

    while(!g_stop) {
        lv_tick_inc(5);
        lv_timer_handler();
        usleep(5000);
    }
    return 0;
}
