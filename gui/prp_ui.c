// Shared PRP recovery UI. See prp_ui.h. Both the device (fbdev) and the desktop SDL
// simulator build from this one file, so the layout/styling never drifts between them.

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "prp_logo.h"
#include "prp_theme.h"
#include "prp_ui.h"

// peacock-maximal type: Instrument Serif headings, Space Mono everything else.
LV_FONT_DECLARE(pk_serif_30);
LV_FONT_DECLARE(pk_serif_44);
LV_FONT_DECLARE(pk_mono_16);
LV_FONT_DECLARE(pk_mono_20);
LV_FONT_DECLARE(pk_mono_26);

static prp_ui_cfg_t g_cfg;

static lv_obj_t *g_log_cont = NULL;
static lv_obj_t *g_log_label = NULL;
static char *g_log_buf = NULL;
static size_t g_log_len = 0;
static size_t g_log_cap = 0;

static lv_obj_t *g_pwr_overlay = NULL;
static lv_img_dsc_t *g_logo_dsc = NULL;

static lv_obj_t *g_tileview = NULL;
static lv_obj_t *g_tile_log = NULL;
static lv_obj_t *g_dots[2] = {NULL, NULL};

static int clampi(int v, int lo, int hi) {
    if(v < lo) return lo;
    if(v > hi) return hi;
    return v;
}

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
    // xorshift64*
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    x *= 2685821657736338717ULL;
    return mottos[(size_t)(x % n)];
}

static bool is_large(int scale_pct, int w, int h) {
    return (h >= 1400 || w >= 800 || scale_pct >= 125);
}

// Instrument Serif for the title (the website's signature heading face).
static const lv_font_t *pick_font_title(int scale_pct, int w, int h) {
    return is_large(scale_pct, w, h) ? &pk_serif_44 : &pk_serif_30;
}

// Space Mono for kickers/labels.
static const lv_font_t *pick_font_hint(int scale_pct, int w, int h) {
    return is_large(scale_pct, w, h) ? &pk_mono_20 : &pk_mono_16;
}

// Space Mono for button labels + console body.
static const lv_font_t *pick_font_body(int scale_pct, int w, int h) {
    return is_large(scale_pct, w, h) ? &pk_mono_26 : &pk_mono_20;
}

void prp_ui_log_append(const char *s, size_t n) {
    if(!g_log_label || !s || n == 0) return;
    if(!g_log_buf) {
        g_log_cap = 8192;
        g_log_buf = (char *)malloc(g_log_cap);
        if(!g_log_buf) return;
        g_log_len = 0;
        g_log_buf[0] = '\0';
    }

    // Keep a bounded log buffer (retain the tail).
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
        if(new_cap < need && g_log_len > 0) {
            size_t keep = g_log_len / 2;
            memmove(g_log_buf, g_log_buf + (g_log_len - keep), keep);
            g_log_len = keep;
            g_log_buf[g_log_len] = '\0';
            need = g_log_len + n + 1;
        }
        if(need <= new_cap) {
            char *nb = (char *)realloc(g_log_buf, new_cap);
            if(nb) {
                g_log_buf = nb;
                g_log_cap = new_cap;
            }
        }
    }

    if(need > g_log_cap) return;
    memcpy(g_log_buf + g_log_len, s, n);
    g_log_len += n;
    g_log_buf[g_log_len] = '\0';

    lv_label_set_text(g_log_label, g_log_buf);
    if(g_log_cont) lv_obj_scroll_to_y(g_log_cont, 32767, LV_ANIM_OFF);
}

void prp_ui_log_appendf(const char *fmt, ...) {
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if(n <= 0) return;
    if((size_t)n >= sizeof(tmp)) n = (int)sizeof(tmp) - 1;
    prp_ui_log_append(tmp, (size_t)n);
}

static void tile_changed_cb(lv_event_t *e) {
    lv_obj_t *tv = lv_event_get_target(e);
    bool on_log = (lv_tileview_get_tile_act(tv) == g_tile_log);
    if(g_dots[0]) lv_obj_set_style_bg_color(g_dots[0], lv_color_hex(on_log ? PK_LINE : PK_TEAL), 0);
    if(g_dots[1]) lv_obj_set_style_bg_color(g_dots[1], lv_color_hex(on_log ? PK_TEAL : PK_LINE), 0);
}

static void ui_btn_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    if(cmd && g_cfg.run_cmd) g_cfg.run_cmd(cmd);
    // After running an action, slide to the output page so the user sees what happened.
    if(g_tileview && g_tile_log) lv_obj_set_tile(g_tileview, g_tile_log, LV_ANIM_ON);
}

static void power_menu_hide(void) {
    if(!g_pwr_overlay) return;
    lv_obj_del(g_pwr_overlay);
    g_pwr_overlay = NULL;
}

static void power_menu_cmd_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    power_menu_hide();
    if(cmd && g_cfg.run_cmd) g_cfg.run_cmd(cmd);
}

static void power_menu_bg_cb(lv_event_t *e) {
    (void)e;
    power_menu_hide();
}

void prp_ui_power_menu_show(void) {
    if(g_pwr_overlay) return;

    lv_obj_t *scr = lv_scr_act();
    const int w = g_cfg.screen_w;
    const int h = g_cfg.screen_h;
    const int scale = clampi(g_cfg.scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 6, 48);
    const lv_font_t *font_title = pick_font_title(scale, w, h);
    const lv_font_t *font_body = pick_font_body(scale, w, h);

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
    lv_obj_set_style_bg_color(dlg, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_bg_opa(dlg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 16, 0);
    lv_obj_set_style_pad_all(dlg, gap, 0);
    lv_obj_set_style_pad_gap(dlg, gap, 0);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *ttl = lv_label_create(dlg);
    lv_label_set_text(ttl, "Power");
    lv_obj_set_style_text_color(ttl, lv_color_hex(PK_CREAM), 0);
    lv_obj_set_style_text_font(ttl, font_title, 0);
    lv_obj_align(ttl, LV_ALIGN_TOP_MID, 0, 0);

    for(int i = 0; i < g_cfg.n_power; i++) {
        const prp_ui_item_t *it = &g_cfg.power_items[i];
        bool ghost = (it->cmd == NULL);   // Cancel = quiet/ghost row
        lv_obj_t *btn = lv_btn_create(dlg);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, clampi(h / 14, 72, 160));
        lv_obj_set_style_bg_color(btn, lv_color_hex(PK_PANEL2), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(ghost ? PK_LINE : PK_TEAL), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
        lv_obj_add_event_cb(btn, power_menu_cmd_cb, LV_EVENT_CLICKED, (void *)it->cmd);

        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, it->label);
        lv_obj_set_style_text_font(lab, font_body, 0);
        lv_obj_set_style_text_color(lab, lv_color_hex(ghost ? PK_DIM : PK_CREAM), 0);
        lv_obj_center(lab);
    }
}

static void appbar_long_press_cb(lv_event_t *e) {
    (void)e;
    prp_ui_power_menu_show();
}

static lv_obj_t *g_why_overlay = NULL;
static lv_obj_t *g_splash = NULL;

/* One action row: icon chip + serif title + mono description + teal chevron. */
static void make_action_row(lv_obj_t *list, int gap, int chip, const char *icon, const char *label,
                            const char *desc, const lv_font_t *title_f, const lv_font_t *desc_f,
                            const lv_font_t *chev_f, lv_event_cb_t cb, void *ud) {
    lv_obj_t *row = lv_btn_create(list);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PK_LINE), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 14, 0);
    lv_obj_set_style_pad_all(row, clampi(gap, 12, 18), 0);
    lv_obj_set_style_pad_column(row, clampi(gap, 12, 18), 0);
    lv_obj_set_style_border_color(row, lv_color_hex(PK_TEAL), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(row, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);

    if(icon) {
        lv_obj_t *chipc = lv_obj_create(row);
        lv_obj_set_size(chipc, chip, chip);
        lv_obj_set_style_bg_color(chipc, lv_color_hex(PK_TEALDK), 0);
        lv_obj_set_style_bg_opa(chipc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(chipc, 0, 0);
        lv_obj_set_style_radius(chipc, 12, 0);
        lv_obj_set_style_pad_all(chipc, 0, 0);
        lv_obj_clear_flag(chipc, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = lv_label_create(chipc);
        lv_label_set_text(ic, icon);
        lv_obj_set_style_text_font(ic, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(PK_TEAL), 0);
        lv_obj_center(ic);
    }

    lv_obj_t *txt = lv_obj_create(row);
    lv_obj_set_height(txt, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(txt, 1);
    lv_obj_set_style_bg_opa(txt, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(txt, 0, 0);
    lv_obj_set_style_pad_all(txt, 0, 0);
    lv_obj_set_style_pad_row(txt, 3, 0);
    lv_obj_set_flex_flow(txt, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(txt, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = lv_label_create(txt);
    lv_label_set_text(t, label);
    lv_obj_set_style_text_font(t, title_f, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(PK_CREAM), 0);
    if(desc) {
        lv_obj_t *d = lv_label_create(txt);
        lv_label_set_text(d, desc);
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(d, desc_f, 0);
        lv_obj_set_style_text_color(d, lv_color_hex(PK_DIM), 0);
    }

    lv_obj_t *chev = lv_label_create(row);
    lv_label_set_text(chev, "→");
    lv_obj_set_style_text_font(chev, chev_f, 0);
    lv_obj_set_style_text_color(chev, lv_color_hex(PK_TEAL), 0);
}

static void why_overlay_hide(void) {
    if(!g_why_overlay) return;
    lv_obj_del(g_why_overlay);
    g_why_overlay = NULL;
}
static void why_close_cb(lv_event_t *e) { (void)e; why_overlay_hide(); }

/* "Why am I in recovery?" — full-screen explainer with a QR to the wiki guide. */
static void why_overlay_show(void) {
    if(g_why_overlay) return;
    lv_obj_t *scr = lv_scr_act();
    const int w = g_cfg.screen_w, h = g_cfg.screen_h;
    const int scale = clampi(g_cfg.scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 8, 40);
    const lv_font_t *ft = pick_font_title(scale, w, h);
    const lv_font_t *fb = pick_font_body(scale, w, h);
    const lv_font_t *fh = pick_font_hint(scale, w, h);

    g_why_overlay = lv_obj_create(scr);
    lv_obj_set_size(g_why_overlay, w, h);
    lv_obj_center(g_why_overlay);
    lv_obj_set_style_bg_color(g_why_overlay, lv_color_hex(PK_BG), 0);
    lv_obj_set_style_bg_grad_color(g_why_overlay, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_grad_dir(g_why_overlay, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_why_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_why_overlay, 0, 0);
    lv_obj_set_style_radius(g_why_overlay, 0, 0);
    lv_obj_set_style_pad_all(g_why_overlay, margin + gap, 0);
    lv_obj_set_flex_flow(g_why_overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_why_overlay, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_why_overlay, gap, 0);

    /* Fixed content width for wrap labels: a LONG_WRAP label at width 100% inside
     * a flex column with cross-align CENTER causes an LVGL 8.3 circular-layout
     * busy-loop. A concrete width breaks the cycle. */
    const int content_w = w - 2 * (margin + gap);

    lv_obj_t *ttl = lv_label_create(g_why_overlay);
    lv_label_set_text(ttl, "Why am I here?");
    lv_obj_set_style_text_font(ttl, ft, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(PK_CREAM), 0);
    lv_obj_set_width(ttl, content_w);
    lv_label_set_long_mode(ttl, LV_LABEL_LONG_WRAP);

    lv_obj_t *body = lv_label_create(g_why_overlay);
    lv_label_set_text(body, g_cfg.reason ? g_cfg.reason :
        "Your device couldn't start the normal system, so it booted into the recovery "
        "environment to keep your data safe. Nothing has been changed automatically. "
        "Use the menu to repair, reinstall, or open a shell.");
    lv_obj_set_width(body, content_w);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(body, fh, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(PK_DIM), 0);

    if(g_cfg.help_url) {
        /* QR created DIRECTLY on the overlay (the qcard wrapper + width-less
         * caption/url labels were what busy-looped the flex layout). The cream
         * light colour + quiet-zone padding give the card look without a wrapper. */
        int qsz = clampi((w < h ? w : h) / 2, 150, 360);
        lv_obj_t *qr = lv_qrcode_create(g_why_overlay, qsz, lv_color_hex(PK_BG), lv_color_hex(PK_CREAM));
        lv_qrcode_update(qr, g_cfg.help_url, (uint32_t)strlen(g_cfg.help_url));
        lv_obj_set_style_bg_color(qr, lv_color_hex(PK_CREAM), 0);
        lv_obj_set_style_bg_opa(qr, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(qr, 12, 0);
        lv_obj_set_style_radius(qr, 12, 0);

        lv_obj_t *cap = lv_label_create(g_why_overlay);
        lv_label_set_text(cap, "SCAN FOR THE RECOVERY GUIDE");
        lv_obj_set_width(cap, content_w);
        lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(cap, fh, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(PK_TEAL), 0);
        lv_obj_set_style_text_letter_space(cap, 2, 0);

        lv_obj_t *url = lv_label_create(g_why_overlay);
        lv_label_set_text(url, g_cfg.help_url);
        lv_obj_set_width(url, content_w);
        lv_obj_set_style_text_align(url, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(url, fh, 0);
        lv_obj_set_style_text_color(url, lv_color_hex(PK_DIM), 0);
    }

    lv_obj_t *btn = lv_btn_create(g_why_overlay);
    lv_obj_set_width(btn, lv_pct(70));
    lv_obj_set_height(btn, clampi(h / 14, 60, 130));
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, why_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Back to menu");
    lv_obj_set_style_text_font(bl, fb, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(PK_CREAM), 0);
    lv_obj_center(bl);
}
static void why_btn_cb(lv_event_t *e) { (void)e; why_overlay_show(); }

static void install_btn_cb(lv_event_t *e) { (void)e; if(g_cfg.on_install) g_cfg.on_install(); }

static void network_btn_cb(lv_event_t *e) { (void)e; if(g_cfg.on_network) g_cfg.on_network(); }

static void splash_continue_cb(lv_event_t *e) {
    (void)e;
    if(!g_splash) return;
    lv_obj_del(g_splash);
    g_splash = NULL;
}

/* "Entered recovery environment" intro shown on top of the menu at startup. */
static void splash_show(void) {
    lv_obj_t *scr = lv_scr_act();
    const int w = g_cfg.screen_w, h = g_cfg.screen_h;
    const int scale = clampi(g_cfg.scale_pct, 50, 200);
    const int margin = clampi((h / 24) * scale / 100, 16, 96);
    const int gap = clampi((h / 40) * scale / 100, 12, 56);
    const lv_font_t *ft = pick_font_title(scale, w, h);
    const lv_font_t *fb = pick_font_body(scale, w, h);
    const lv_font_t *fh = pick_font_hint(scale, w, h);

    g_splash = lv_obj_create(scr);
    lv_obj_set_size(g_splash, w, h);
    lv_obj_center(g_splash);
    lv_obj_set_style_bg_color(g_splash, lv_color_hex(PK_BG), 0);
    lv_obj_set_style_bg_grad_color(g_splash, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_grad_dir(g_splash, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(g_splash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_splash, 0, 0);
    lv_obj_set_style_radius(g_splash, 0, 0);
    lv_obj_set_style_pad_all(g_splash, margin, 0);
    lv_obj_set_flex_flow(g_splash, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_splash, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(g_splash, gap, 0);

    if(g_logo_dsc) {
        lv_obj_t *img = lv_img_create(g_splash);
        lv_img_set_src(img, g_logo_dsc);
        lv_img_set_antialias(img, true);
        int ih = (int)g_logo_dsc->header.h;
        int target = clampi(h / 5, 80, 320);
        uint32_t z = (ih > 0) ? (uint32_t)target * 256u / (uint32_t)ih : 256u;
        if(z < 32u) z = 32u;
        if(z > 1024u) z = 1024u;
        lv_img_set_zoom(img, (uint16_t)z);
    }

    lv_obj_t *eb = lv_label_create(g_splash);
    lv_label_set_text(eb, "RECOVERY ENVIRONMENT");
    lv_obj_set_style_text_font(eb, fh, 0);
    lv_obj_set_style_text_color(eb, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_text_letter_space(eb, 3, 0);

    lv_obj_t *ttl = lv_label_create(g_splash);
    lv_label_set_text(ttl, "You're in recovery");
    lv_obj_set_style_text_font(ttl, ft, 0);
    lv_obj_set_style_text_color(ttl, lv_color_hex(PK_CREAM), 0);

    lv_obj_t *body = lv_label_create(g_splash);
    lv_label_set_text(body,
        "Your phone started the recovery environment instead of the usual system. "
        "From here you can repair it, reinstall, or get help. Nothing has been changed yet.");
    lv_obj_set_width(body, lv_pct(90));
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(body, fh, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(PK_DIM), 0);

    lv_obj_t *btn = lv_btn_create(g_splash);
    lv_obj_set_width(btn, lv_pct(70));
    lv_obj_set_height(btn, clampi(h / 13, 64, 140));
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, splash_continue_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(btn);
    lv_label_set_text(bl, "Continue");
    lv_obj_set_style_text_font(bl, fb, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(PK_CREAM), 0);
    lv_obj_center(bl);
}

void prp_ui_build(const prp_ui_cfg_t *cfg) {
    g_cfg = *cfg;

    const int w = g_cfg.screen_w;
    const int h = g_cfg.screen_h;
    const int scale = clampi(g_cfg.scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 6, 48);
    const int appbar_pad = clampi(gap / 2, 8, 28);

    const lv_font_t *font_title = pick_font_title(scale, w, h);
    const lv_font_t *font_hint = pick_font_hint(scale, w, h);
    const lv_font_t *font_body = pick_font_body(scale, w, h);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(PK_BG), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x0A1018), 0);   // subtle ambient depth
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    /* On real fbdev, leaving bg_opa at default can show whatever the kernel/boot stage
       left in the framebuffer (often red); force opaque. */
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(PK_CREAM), 0);
    lv_obj_set_style_text_font(scr, font_body, 0);

    /* AppBar-style header */
    const int appbar_extra = clampi((h / 80) * scale / 100, 6, 28);
    const int appbar_h = clampi((int)font_title->line_height + appbar_pad * 2 + appbar_extra, 72, 260);
    lv_obj_t *appbar = lv_obj_create(scr);
    lv_obj_set_size(appbar, w, appbar_h);
    lv_obj_align(appbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(appbar, lv_color_hex(PK_BG), 0);
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

    /* Iridescent accent line under the header. */
    const int sep_h = (scale >= 140) ? 3 : 2;
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, w, sep_h);
    lv_obj_align(sep, LV_ALIGN_TOP_MID, 0, appbar_h);
    lv_obj_set_style_bg_color(sep, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_bg_grad_color(sep, lv_color_hex(PK_VIOLET), 0);
    lv_obj_set_style_bg_grad_dir(sep, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);

    if(!g_logo_dsc && g_cfg.logo_paths) {
        g_logo_dsc = prp_logo_try_load(g_cfg.logo_paths);
    }

    lv_obj_t *title = lv_label_create(appbar);
    lv_label_set_text(title, "PRP Recovery");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(PK_CREAM), 0);
    lv_obj_align(title, LV_ALIGN_RIGHT_MID, -appbar_pad, 0);

    const int inner_w = w - 2 * margin;

    if(g_logo_dsc) {
        const int max_h = appbar_h - 6;
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

    // Hidden "motto" below the AppBar contents. Visible only if the header is scrolled.
    lv_obj_t *motto = lv_label_create(appbar);
    lv_label_set_text(motto, pick_motto());
    lv_label_set_long_mode(motto, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(motto, w - 2 * appbar_pad);
    lv_obj_set_style_text_font(motto, font_hint, 0);
    lv_obj_set_style_text_color(motto, lv_color_hex(PK_DIM), 0);
    lv_obj_align(motto, LV_ALIGN_TOP_MID, 0, appbar_h + appbar_pad);

    const int body_y = appbar_h + sep_h;
    const int body_h = h - body_y;
    const int kicker_h = (int)font_hint->line_height + clampi(margin / 2, 6, 24);
    const int dots_h = clampi(margin, 18, 40);

    /* Swipeable body: page 0 = actions, page 1 = output log. The status bar stays fixed. */
    lv_obj_t *tv = lv_tileview_create(scr);
    g_tileview = tv;
    lv_obj_set_size(tv, w, body_h);
    lv_obj_align(tv, LV_ALIGN_TOP_MID, 0, body_y);
    lv_obj_set_style_bg_opa(tv, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_event_cb(tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *tile_main = lv_tileview_add_tile(tv, 0, 0, LV_DIR_RIGHT);
    g_tile_log = lv_tileview_add_tile(tv, 1, 0, LV_DIR_LEFT);
    lv_obj_set_style_pad_all(tile_main, margin, 0);
    lv_obj_set_style_pad_all(g_tile_log, margin, 0);
    lv_obj_set_scrollbar_mode(tile_main, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scrollbar_mode(g_tile_log, LV_SCROLLBAR_MODE_OFF);

    /* ---------- page 0: actions ---------- */
    lv_obj_t *eb = lv_obj_create(tile_main);
    lv_obj_set_size(eb, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(eb, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(eb, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(eb, 0, 0);
    lv_obj_set_style_pad_all(eb, 0, 0);
    lv_obj_set_style_pad_column(eb, 8, 0);
    lv_obj_set_flex_flow(eb, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(eb, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(eb, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *pip = lv_obj_create(eb);
    lv_obj_set_size(pip, 8, 8);
    lv_obj_set_style_radius(pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(pip, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_border_width(pip, 0, 0);
    lv_obj_clear_flag(pip, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *eyebrow = lv_label_create(eb);
    lv_label_set_text(eyebrow, "CHOOSE AN ACTION");
    lv_obj_set_style_text_font(eyebrow, font_hint, 0);
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_text_letter_space(eyebrow, 3, 0);

    const int btn_count = g_cfg.n_buttons;
    const int chip = clampi((int)font_body->line_height + 20, 44, 76);

    /* Vertical list of action rows (icon chip + title/desc + chevron). Hugs content. */
    lv_obj_t *list = lv_obj_create(tile_main);
    lv_obj_set_size(list, inner_w, body_h - 2 * margin - kicker_h - dots_h);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, kicker_h);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, clampi(gap, 10, 18), 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

    /* Primary action: install a fresh system on-device. */
    if(g_cfg.on_install) {
        make_action_row(list, gap, chip, LV_SYMBOL_DOWNLOAD, "Install PeacockOS",
                        "Set up a fresh system on this device", &pk_serif_30, font_hint, font_body,
                        install_btn_cb, NULL);
    }

    /* Networking: connect to Wi-Fi (for installs, SSH, remote recovery). */
    if(g_cfg.on_network) {
        make_action_row(list, gap, chip, LV_SYMBOL_WIFI, "Connect to Wi-Fi",
                        "Join a network for install or remote access", &pk_serif_30, font_hint, font_body,
                        network_btn_cb, NULL);
    }

    /* Then: explain the situation + link to the recovery guide. */
    make_action_row(list, gap, chip, LV_SYMBOL_WARNING, "Why am I in recovery?",
                    "What happened, and how to get help", &pk_serif_30, font_hint, font_body,
                    why_btn_cb, NULL);

    for(int i = 0; i < btn_count; i++) {
        const prp_ui_item_t *it = &g_cfg.buttons[i];
        make_action_row(list, gap, chip, it->icon, it->label, it->desc,
                        &pk_serif_30, font_hint, font_body, ui_btn_cb, (void *)it->cmd);
    }

    /* page indicator dots (swipe affordance), persistent over both pages */
    for(int i = 0; i < 2; i++) {
        g_dots[i] = lv_obj_create(scr);
        lv_obj_set_size(g_dots[i], 10, 10);
        lv_obj_set_style_radius(g_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(g_dots[i], 0, 0);
        lv_obj_set_style_bg_color(g_dots[i], lv_color_hex(i == 0 ? PK_TEAL : PK_LINE), 0);
        lv_obj_align(g_dots[i], LV_ALIGN_BOTTOM_MID, (i == 0 ? -9 : 9), -clampi(margin / 2, 6, 16));
    }

    /* ---------- page 1: output ---------- */
    lv_obj_t *log_kicker = lv_label_create(g_tile_log);
    lv_label_set_text(log_kicker, "OUTPUT");
    lv_obj_set_style_text_font(log_kicker, font_hint, 0);
    lv_obj_set_style_text_color(log_kicker, lv_color_hex(PK_DIM), 0);
    lv_obj_set_style_text_letter_space(log_kicker, 3, 0);
    lv_obj_align(log_kicker, LV_ALIGN_TOP_LEFT, 0, 0);

    g_log_cont = lv_obj_create(g_tile_log);
    lv_obj_set_size(g_log_cont, inner_w, body_h - 2 * margin - kicker_h - dots_h);
    lv_obj_align(g_log_cont, LV_ALIGN_TOP_MID, 0, kicker_h);
    lv_obj_set_style_bg_color(g_log_cont, lv_color_hex(PK_PANEL2), 0);
    lv_obj_set_style_bg_opa(g_log_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_log_cont, lv_color_hex(PK_LINE), 0);
    lv_obj_set_style_border_width(g_log_cont, 1, 0);
    lv_obj_set_style_radius(g_log_cont, 10, 0);
    lv_obj_set_style_pad_all(g_log_cont, 12, 0);
    lv_obj_set_scroll_dir(g_log_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_log_cont, LV_SCROLLBAR_MODE_AUTO);

    g_log_label = lv_label_create(g_log_cont);
    lv_obj_set_width(g_log_label, lv_pct(100));
    lv_label_set_long_mode(g_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(g_log_label, font_hint, 0);
    lv_obj_set_style_text_color(g_log_label, lv_color_hex(PK_CREAM), 0);
    lv_label_set_text(g_log_label, "");

    /* Greet the user on entry (they may have landed here from a failed boot). */
    if(!getenv("PRP_NO_SPLASH")) splash_show();
}
