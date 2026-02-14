// PRP GUI host simulator using LVGL + lv_drivers SDL display/input.
// This lets us iterate on the PRP GUI without flashing a phone.
//
// Build via: scripts/build-gui-host.sh (requires SDL2 dev libs)

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "lv_drivers/sdl/sdl.h"

// Reuse the same PNG->RGB565 loader as the device build so layout is identical.
#include "prp_logo.h"

static volatile sig_atomic_t g_stop = 0;
static int g_scale_pct = 100;
static lv_img_dsc_t *g_logo_dsc = NULL;

static lv_obj_t *g_log_cont = NULL;
static lv_obj_t *g_log_label = NULL;
static char *g_log_buf = NULL;
static size_t g_log_len = 0;
static size_t g_log_cap = 0;

static lv_obj_t *g_pwr_overlay = NULL;

typedef struct {
    int fd;
    pid_t pid;
    bool running;
} prp_job_t;

static prp_job_t g_job = {.fd = -1, .pid = -1, .running = false};

static const char *pick_motto(void) {
    static const char *mottos[] = {
        "Put it all on the line",
        "A series of dead ends",
        "Hush now",
        "Memories all plagued by the touch",
        "Crimson fate",
        "Let go",
        "Won't matter, fall or rise",
        "We needed a reason, fetch the gun, it's the season",
    };
    const size_t n = sizeof(mottos) / sizeof(mottos[0]);
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t x = ((uint64_t)ts.tv_sec << 32) ^ (uint64_t)ts.tv_nsec;
    x ^= (uint64_t)(uintptr_t)&ts;
    x ^= (uint64_t)getpid();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 2685821657736338717ULL;
    return mottos[(size_t)(x % n)];
}

static void on_sig(int sig) {
    (void)sig;
    g_stop = 1;
}

static void log_append_raw(const char *s, size_t n) {
    if(!g_log_label || !s || n == 0) return;
    if(!g_log_buf) {
        g_log_cap = 8192;
        g_log_buf = (char *)malloc(g_log_cap);
        if(!g_log_buf) return;
        g_log_len = 0;
        g_log_buf[0] = '\0';
    }

    const size_t max_keep = 32768;
    if(g_log_len > max_keep) {
        size_t keep = max_keep / 2;
        memmove(g_log_buf, g_log_buf + (g_log_len - keep), keep);
        g_log_len = keep;
        g_log_buf[g_log_len] = '\0';
    }

    size_t need = g_log_len + n + 1;
    if(need > g_log_cap) {
        size_t new_cap = g_log_cap;
        while(new_cap < need) new_cap *= 2;
        if(new_cap > 131072) new_cap = 131072;
        char *nb = (char *)realloc(g_log_buf, new_cap);
        if(nb) {
            g_log_buf = nb;
            g_log_cap = new_cap;
        }
    }
    if(need > g_log_cap) return;

    memcpy(g_log_buf + g_log_len, s, n);
    g_log_len += n;
    g_log_buf[g_log_len] = '\0';
    lv_label_set_text(g_log_label, g_log_buf);
    if(g_log_cont) lv_obj_scroll_to_y(g_log_cont, 32767, LV_ANIM_OFF);
}

static void log_appendf(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    if((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    log_append_raw(tmp, (size_t)n);
}

static int clampi(int v, int lo, int hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

static const lv_font_t *pick_font_title(int scale_pct, int w, int h) {
    int tier = 0;
    if(scale_pct >= 140) tier = 2;
    else if(scale_pct >= 105) tier = 1;
    if(h >= 1600 || w >= 900) tier++;
    if(tier <= 0) return &lv_font_montserrat_24;
    if(tier == 1) return &lv_font_montserrat_28;
    return &lv_font_montserrat_32;
}

static const lv_font_t *pick_font_hint(int scale_pct, int w, int h) {
    int tier = 0;
    if(scale_pct >= 140) tier = 2;
    else if(scale_pct >= 105) tier = 1;
    if(h >= 1600 || w >= 900) tier++;
    if(tier <= 0) return &lv_font_montserrat_16;
    if(tier == 1) return &lv_font_montserrat_20;
    return &lv_font_montserrat_24;
}

static const lv_font_t *pick_font_body(int scale_pct, int w, int h) {
    int tier = 0;
    if(scale_pct >= 140) tier = 2;
    else if(scale_pct >= 105) tier = 1;
    if(h >= 1600 || w >= 900) tier++;
    if(tier <= 0) return &lv_font_montserrat_20;
    if(tier == 1) return &lv_font_montserrat_24;
    return &lv_font_montserrat_28;
}

static void btn_cmd_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if(!cmd) return;
    if(g_job.running) {
        log_appendf("\n[busy] previous command still running (pid=%d)\n", (int)g_job.pid);
        return;
    }
    log_appendf("\n$ %s\n", cmd);

    int pfd[2];
    if(pipe(pfd) != 0) {
        log_appendf("[err] pipe failed: %s\n", strerror(errno));
        return;
    }
    (void)fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if(pid < 0) {
        log_appendf("[err] fork failed: %s\n", strerror(errno));
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if(pid == 0) {
        (void)dup2(pfd[1], STDOUT_FILENO);
        (void)dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    g_job.fd = pfd[0];
    g_job.pid = pid;
    g_job.running = true;
}

static void btn_quit_cb(lv_event_t *e) {
    (void)e;
    g_stop = 1;
}

static void power_menu_hide(void);

static void power_menu_cmd_cb(lv_event_t *e) {
    const char *label = (const char *)lv_event_get_user_data(e);
    power_menu_hide();
    if(!label) return;
    log_appendf("\n[host] %s (simulated)\n", label);
}

static void power_menu_bg_cb(lv_event_t *e) {
    (void)e;
    power_menu_hide();
}

static void power_menu_hide(void) {
    if(!g_pwr_overlay) return;
    lv_obj_del(g_pwr_overlay);
    g_pwr_overlay = NULL;
}

static void power_menu_show(int w, int h) {
    if(g_pwr_overlay) return;

    const int scale = clampi(g_scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 6, 48);
    const lv_font_t *font_title = pick_font_title(scale, w, h);
    const lv_font_t *font_body = pick_font_body(scale, w, h);

    lv_obj_t *scr = lv_scr_act();
    g_pwr_overlay = lv_obj_create(scr);
    lv_obj_set_size(g_pwr_overlay, w, h);
    lv_obj_align(g_pwr_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_pwr_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_pwr_overlay, LV_OPA_60, 0);
    lv_obj_set_style_border_width(g_pwr_overlay, 0, 0);
    lv_obj_set_style_radius(g_pwr_overlay, 0, 0);
    lv_obj_add_event_cb(g_pwr_overlay, power_menu_bg_cb, LV_EVENT_CLICKED, NULL);

    const int dlg_w = clampi(w - 2 * margin, 360, 720);
    const int dlg_h = clampi(h / 3, 240, 560);
    lv_obj_t *dlg = lv_obj_create(g_pwr_overlay);
    lv_obj_set_size(dlg, dlg_w, dlg_h);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_white(), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 12, 0);
    lv_obj_set_style_pad_all(dlg, gap, 0);
    lv_obj_set_style_pad_gap(dlg, gap, 0);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ttl = lv_label_create(dlg);
    lv_label_set_text(ttl, "Power");
    lv_obj_set_style_text_color(ttl, lv_color_white(), 0);
    lv_obj_set_style_text_font(ttl, font_title, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);

    const char *acts[] = {"Reboot", "Power off", "Cancel"};
    for(size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        lv_obj_t *btn = lv_btn_create(dlg);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, clampi(h / 14, 72, 160));
        lv_obj_set_style_bg_color(btn, lv_color_make(0x10, 0x10, 0x10), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, power_menu_cmd_cb, LV_EVENT_CLICKED, (void *)acts[i]);

        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, acts[i]);
        lv_obj_set_style_text_font(lab, font_body, 0);
        lv_obj_set_style_text_color(lab, lv_color_white(), 0);
        lv_obj_center(lab);
    }
}

static void appbar_long_press_cb(lv_event_t *e) {
    (void)e;
    int w = lv_disp_get_hor_res(NULL);
    int h = lv_disp_get_ver_res(NULL);
    power_menu_show(w, h);
}

static void build_ui(int w, int h) {
    const int scale = clampi(g_scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 6, 48);
    const int appbar_pad = clampi(gap / 2, 8, 28);

    const lv_font_t *font_title = pick_font_title(scale, w, h);
    const lv_font_t *font_hint = pick_font_hint(scale, w, h);
    const lv_font_t *font_body = pick_font_body(scale, w, h);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_set_style_text_font(scr, font_body, 0);

    /* AppBar-style header */
    const int appbar_extra = clampi((h / 80) * scale / 100, 6, 28);
    const int appbar_h = clampi((int)font_title->line_height + appbar_pad * 2 + appbar_extra, 72, 260);
    lv_obj_t *appbar = lv_obj_create(scr);
    lv_obj_set_size(appbar, w, appbar_h);
    lv_obj_align(appbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(appbar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(appbar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(appbar, 0, 0);
    lv_obj_set_style_radius(appbar, 0, 0);
    lv_obj_set_style_pad_left(appbar, appbar_pad, 0);
    lv_obj_set_style_pad_right(appbar, appbar_pad, 0);
    lv_obj_set_style_pad_top(appbar, appbar_pad, 0);
    lv_obj_set_style_pad_bottom(appbar, appbar_pad, 0);
    lv_obj_set_scroll_dir(appbar, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(appbar, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(appbar, appbar_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    /* Thin separator under the header. */
    const int sep_h = (scale >= 140) ? 3 : 2;
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, w, sep_h);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, appbar_h);
    lv_obj_set_style_bg_color(sep, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    // Optional logo image loaded from disk.
    if(!g_logo_dsc) {
        const char *envp = getenv("PRP_GUI_LOGO");
        const char *paths[8];
        size_t pi = 0;
        if(envp && *envp) paths[pi++] = envp;
        paths[pi++] = "header_logo.png";
        paths[pi++] = "logo_header.png";
        paths[pi++] = "../header_logo.png";
        paths[pi++] = "/etc/prp/header_logo.png";
        paths[pi] = NULL;
        g_logo_dsc = prp_logo_try_load(paths);
    }

    lv_obj_t *title = lv_label_create(appbar);
    lv_label_set_text(title, "PRP Recovery");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -appbar_pad, 0);

    if(g_logo_dsc) {
        const int max_h = appbar_h - 6;
        const int inner_w = w - 2 * margin;
        const int max_w = clampi(inner_w / 3, 96, inner_w / 2);
        const int iw = (int)g_logo_dsc->header.w;
        const int ih = (int)g_logo_dsc->header.h;

        uint32_t z = 256;
        if(iw > 0 && ih > 0) {
            uint32_t zx = (uint32_t)max_w * 256u / (uint32_t)iw;
            uint32_t zy = (uint32_t)max_h * 256u / (uint32_t)ih;
            z = zx < zy ? zx : zy;
            if(z > 768u) z = 768u;
            if(z < 64u) z = 64u;
        }

        lv_obj_t *img = lv_img_create(appbar);
        lv_img_set_src(img, g_logo_dsc);
        lv_img_set_antialias(img, false);
        lv_img_set_size_mode(img, LV_IMG_SIZE_MODE_REAL);
        lv_img_set_zoom(img, (uint16_t)z);
        lv_obj_align(img, LV_ALIGN_LEFT_MID, appbar_pad, 0);
    }

    // Hidden "motto" below the AppBar contents. Visible only if the user scrolls the header.
    lv_obj_t *motto = lv_label_create(appbar);
    lv_label_set_text(motto, pick_motto());
    lv_label_set_long_mode(motto, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(motto, w - 2 * appbar_pad);
    lv_obj_set_style_text_font(motto, font_hint, 0);
    lv_obj_set_style_text_color(motto, lv_color_make(0xE0, 0xE0, 0xE0), 0);
    lv_obj_align(motto, LV_ALIGN_TOP_MID, 0, appbar_h + appbar_pad);

    const int inner_w = w - 2 * margin;
    const int top_y = appbar_h + sep_h + margin;

    const int log_h_default = clampi((h / 3) * scale / 100, 120, h / 2);

    int btn_h_want = clampi((h / 8) * scale / 100, 96, 320);
    const int btn_area_h_want = btn_h_want * 2 + gap;

    int log_h_max_for_btn = (h - margin) - top_y - gap - btn_area_h_want;
    if(log_h_max_for_btn > h / 2) log_h_max_for_btn = h / 2;

    int log_h = log_h_default;
    if(log_h_max_for_btn >= 120) {
        if(log_h > log_h_max_for_btn) log_h = log_h_max_for_btn;
    } else {
        log_h = 120;
        int available = (h - margin - log_h) - top_y - gap;
        int btn_h_max = (available - gap) / 2;
        btn_h_want = clampi(btn_h_want, 64, btn_h_max > 0 ? btn_h_max : 64);
    }

    const int btn_h = btn_h_want;
    const int btn_area_h = btn_h * 2 + gap;

    g_log_cont = lv_obj_create(scr);
    lv_obj_set_size(g_log_cont, inner_w, log_h);
    lv_obj_align(g_log_cont, LV_ALIGN_BOTTOM_MID, 0, -margin);
    lv_obj_set_style_bg_color(g_log_cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_log_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_log_cont, lv_color_make(0x40, 0x40, 0x40), 0);
    lv_obj_set_style_border_width(g_log_cont, 2, 0);
    lv_obj_set_style_radius(g_log_cont, 6, 0);
    lv_obj_set_style_pad_all(g_log_cont, 10, 0);
    lv_obj_set_scroll_dir(g_log_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_log_cont, LV_SCROLLBAR_MODE_AUTO);

    g_log_label = lv_label_create(g_log_cont);
    lv_obj_set_width(g_log_label, lv_pct(100));
    lv_label_set_long_mode(g_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_log_label, font_hint, 0);
    lv_obj_set_style_text_color(g_log_label, lv_color_white(), 0);
    lv_label_set_text(g_log_label, "");

    lv_obj_t *btn_area = lv_obj_create(scr);
    lv_obj_set_size(btn_area, inner_w, btn_area_h);
    lv_obj_align(btn_area, LV_ALIGN_TOP_MID, 0, top_y);
    lv_obj_set_style_bg_color(btn_area, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn_area, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn_area, 0, 0);
    lv_obj_set_style_pad_all(btn_area, 0, 0);
    lv_obj_set_flex_flow(btn_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(btn_area, gap, 0);

    struct {
        const char *label;
        const char *cmd;
    } buttons[] = {
        {"Open Shell", "setsid -f sh -lc 'xterm -e sh || alacritty -e sh || gnome-terminal -- sh || konsole -e sh'"},
        {"Print Config", "sh -lc 'echo PRP_GUI_SCALE=$PRP_GUI_SCALE'"},
        {"Quit", "true"},
        {"No-op", "true"},
    };

    const int btn_w = (inner_w - gap) / 2;

    size_t idx = 0;
    for(int row = 0; row < 2; row++) {
        lv_obj_t *rowc = lv_obj_create(btn_area);
        lv_obj_set_style_bg_color(rowc, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(rowc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rowc, 0, 0);
        lv_obj_set_style_pad_all(rowc, 0, 0);
        lv_obj_set_size(rowc, lv_pct(100), btn_h);
        lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(rowc, gap, 0);

        for(int col = 0; col < 2; col++) {
            lv_obj_t *btn = lv_btn_create(rowc);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_set_style_bg_color(btn, lv_color_make(0x10, 0x10, 0x10), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(btn, lv_color_white(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_radius(btn, 8, 0);

            const char *cmd = buttons[idx].cmd;
            if(strcmp(buttons[idx].label, "Quit") == 0) cmd = NULL;

            lv_obj_add_event_cb(btn, btn_cmd_cb, LV_EVENT_CLICKED, (void *)cmd);
            if(strcmp(buttons[idx].label, "Quit") == 0) {
                lv_obj_add_event_cb(btn, btn_quit_cb, LV_EVENT_CLICKED, NULL);
            }

            lv_obj_t *lab = lv_label_create(btn);
            lv_label_set_text(lab, buttons[idx].label);
            lv_obj_set_style_text_font(lab, font_body, 0);
            lv_obj_center(lab);

            idx++;
        }
    }
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--scale PCT]\\n", argv0);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    const char *env_scale = getenv("PRP_GUI_SCALE");
    if(env_scale && *env_scale) g_scale_pct = atoi(env_scale);

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if(strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            g_scale_pct = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    lv_init();
    sdl_init();

    /* Display */
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
    lv_disp_set_bg_color(disp, lv_color_black());
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);

    /* Input: mouse as touch */
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    build_ui(SDL_HOR_RES, SDL_VER_RES);

    while(!g_stop) {
        if(g_job.running && g_job.fd >= 0) {
            char buf[512];
            for(;;) {
                ssize_t n = read(g_job.fd, buf, sizeof(buf));
                if(n > 0) {
                    log_append_raw(buf, (size_t)n);
                    continue;
                }
                if(n == 0) {
                    int st = 0;
                    (void)waitpid(g_job.pid, &st, WNOHANG);
                    close(g_job.fd);
                    g_job.fd = -1;
                    g_job.running = false;
                    if(WIFEXITED(st)) log_appendf("[exit] code=%d\n", WEXITSTATUS(st));
                    else if(WIFSIGNALED(st)) log_appendf("[exit] signal=%d\n", WTERMSIG(st));
                    else log_appendf("[exit] done\n");
                    break;
                }
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                log_appendf("[err] read: %s\n", strerror(errno));
                break;
            }
        }
        lv_tick_inc(5);
        lv_timer_handler();
        usleep(5000);
    }

    return 0;
}
