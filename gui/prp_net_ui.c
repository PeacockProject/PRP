// prp_net_ui.c — standalone Wi-Fi connect overlay (see prp_net_ui.h).
//
// On device this forks `prp-net scan` / `prp-net connect <ssid> <psk>` and polls
// their result files (/tmp/prp-net-{scan,status}) from an lv_timer, so the GUI
// never blocks. With no prp-net binary (the SDL sim) it falls back to the mock
// SSID list and a simulated connect, so the flow stays exercisable.

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "prp_theme.h"
#include "prp_net_ui.h"

LV_FONT_DECLARE(pk_serif_30);
LV_FONT_DECLARE(pk_serif_44);
LV_FONT_DECLARE(pk_mono_16);
LV_FONT_DECLARE(pk_mono_20);

#define SCAN_FILE   "/tmp/prp-net-scan"
#define STATUS_FILE "/tmp/prp-net-status"

static struct {
    lv_obj_t *overlay, *ssid_dd, *psk_ta, *cc_ta, *status_lbl, *kb, *scan_btn_lbl;
    int w, h, scale;
    const char *mock_ssids;
    const lv_font_t *f_title, *f_body, *f_small;

    pid_t scan_pid, connect_pid;
    lv_timer_t *scan_timer, *connect_timer;
    int scan_ticks, connect_ticks;

    int connected;
    char status[128];
    char ssid_opts[2048];
} N;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void set_status(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(N.status, sizeof(N.status), fmt, ap);
    va_end(ap);
    if(N.status_lbl) lv_label_set_text(N.status_lbl, N.status);
}

int prp_net_ui_connected(void) { return N.connected; }
const char *prp_net_ui_status(void) { return N.status[0] ? N.status : "Not connected"; }

/* Locate the prp-net helper; absent in the sim → mock mode. */
static int prp_net_present(void) {
    static const char *paths[] = { "/usr/bin/prp-net", "/sbin/prp-net", "/bin/prp-net" };
    for(size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if(access(paths[i], X_OK) == 0) return 1;
    }
    return 0;
}

/* fork+exec prp-net with up to 3 args (NULL-terminated by caller via empties). */
static pid_t spawn_prpnet(const char *a1, const char *a2, const char *a3) {
    pid_t p = fork();
    if(p < 0) return -1;
    if(p == 0) {
        if(a3 && *a3)      execlp("prp-net", "prp-net", a1, a2, a3, (char *)NULL);
        else if(a2 && *a2) execlp("prp-net", "prp-net", a1, a2, (char *)NULL);
        else               execlp("prp-net", "prp-net", a1, (char *)NULL);
        _exit(127);
    }
    return p;
}

static int read_file_small(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if(fd < 0) return -1;
    ssize_t n = read(fd, buf, cap - 1);
    close(fd);
    if(n < 0) n = 0;
    buf[n] = '\0';
    return (int)n;
}

/* ---- on-screen keyboard ---- */
static void ta_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(N.kb, ta);
        lv_obj_clear_flag(N.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(N.kb);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(N.kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(N.kb, NULL);
    }
}
static void kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(N.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- scan ---- */
static void scan_finish_from_file(void) {
    char buf[2048];
    int count = 0;
    if(read_file_small(SCAN_FILE, buf, sizeof(buf)) > 0 && buf[0]) {
        /* file is already one SSID per line; count + reuse as dropdown options */
        size_t len = strlen(buf);
        while(len && (buf[len - 1] == '\n' || buf[len - 1] == ' ')) buf[--len] = '\0';
        for(size_t i = 0; i < len; i++) if(buf[i] == '\n') count++;
        if(len) count++;
        (void)snprintf(N.ssid_opts, sizeof(N.ssid_opts), "%s", buf);
    }
    if(count > 0) {
        lv_dropdown_set_options(N.ssid_dd, N.ssid_opts);
        set_status("Found %d network%s — pick one", count, count == 1 ? "" : "s");
    } else {
        lv_dropdown_set_options(N.ssid_dd, "(no networks found)");
        set_status("No networks found — try Scan again");
    }
}
static void scan_poll(lv_timer_t *t) {
    int st;
    pid_t r = (N.scan_pid > 0) ? waitpid(N.scan_pid, &st, WNOHANG) : -1;
    if(r == N.scan_pid || r < 0) {
        scan_finish_from_file();
        lv_timer_del(t);
        N.scan_timer = NULL;
        N.scan_pid = 0;
        return;
    }
    if(++N.scan_ticks > 24) {  /* ~12s */
        set_status("Scan timed out");
        lv_timer_del(t);
        N.scan_timer = NULL;
        N.scan_pid = 0;
    }
}
static void do_scan(void) {
    if(N.scan_timer) return;
    if(!prp_net_present()) {
        lv_dropdown_set_options(N.ssid_dd, (N.mock_ssids && *N.mock_ssids)
                                           ? N.mock_ssids : "(no networks)");
        set_status("Select a network (simulated)");
        return;
    }
    (void)unlink(SCAN_FILE);
    N.scan_pid = spawn_prpnet("scan", NULL, NULL);
    if(N.scan_pid <= 0) { set_status("Could not start scan"); return; }
    N.scan_ticks = 0;
    set_status("Scanning for networks…");
    N.scan_timer = lv_timer_create(scan_poll, 500, NULL);
}
static void scan_btn_cb(lv_event_t *e) { (void)e; do_scan(); }

/* ---- connect ---- */
static void connect_poll(lv_timer_t *t) {
    int st;
    pid_t r = (N.connect_pid > 0) ? waitpid(N.connect_pid, &st, WNOHANG) : -1;
    if(r == N.connect_pid || r < 0) {
        char buf[128];
        lv_timer_del(t);
        N.connect_timer = NULL;
        N.connect_pid = 0;
        if(read_file_small(STATUS_FILE, buf, sizeof(buf)) > 0 &&
           strncmp(buf, "ok", 2) == 0) {
            char *ip = buf + 2;
            while(*ip == ' ') ip++;
            char *nl = strchr(ip, '\n'); if(nl) *nl = '\0';
            N.connected = 1;
            set_status("Connected · %s", ip[0] ? ip : "online");
        } else if(strncmp(buf, "fail need-country", 17) == 0) {
            /* Associated, but a 5GHz/DFS channel needs a regulatory country to
             * transmit. Reveal the region field and ask — don't blame the pass. */
            if(N.cc_ta) {
                lv_obj_clear_flag(N.cc_ta, LV_OBJ_FLAG_HIDDEN);
                lv_obj_scroll_to_view(N.cc_ta, LV_ANIM_OFF);
            }
            set_status("This 5 GHz network needs your region — enter your 2-letter "
                       "country code (e.g. US, DE, TR) above and reconnect.");
        } else {
            set_status("Couldn't connect — check the password");
        }
        return;
    }
    if(++N.connect_ticks > 60) {  /* ~30s */
        set_status("Connection timed out");
        lv_timer_del(t);
        N.connect_timer = NULL;
        N.connect_pid = 0;
    }
}
static void connect_btn_cb(lv_event_t *e) {
    (void)e;
    char ssid[96];
    lv_dropdown_get_selected_str(N.ssid_dd, ssid, sizeof(ssid));
    if(!ssid[0] || ssid[0] == '(') { set_status("Scan + pick a network first"); return; }
    const char *psk = N.psk_ta ? lv_textarea_get_text(N.psk_ta) : "";

    /* If the region field is showing and filled, persist the country first
     * (fast, finishes before connect) so this attempt can use 5GHz/DFS. */
    if(N.cc_ta && !lv_obj_has_flag(N.cc_ta, LV_OBJ_FLAG_HIDDEN) && prp_net_present()) {
        const char *cc = lv_textarea_get_text(N.cc_ta);
        if(cc && cc[0]) {
            pid_t p = spawn_prpnet("set-country", cc, NULL);
            if(p > 0) { int st; (void)waitpid(p, &st, 0); }
        }
    }

    if(!prp_net_present()) {
        N.connected = 1;
        set_status("Connected · %s (simulated)", ssid);
        return;
    }
    if(N.connect_timer) return;
    (void)unlink(STATUS_FILE);
    N.connect_pid = spawn_prpnet("connect", ssid, psk);
    if(N.connect_pid <= 0) { set_status("Could not start connect"); return; }
    N.connect_ticks = 0;
    set_status("Connecting to %s…", ssid);
    N.connect_timer = lv_timer_create(connect_poll, 500, NULL);
}

/* ---- teardown ---- */
static void net_close(void) {
    if(N.scan_timer) { lv_timer_del(N.scan_timer); N.scan_timer = NULL; }
    if(N.connect_timer) { lv_timer_del(N.connect_timer); N.connect_timer = NULL; }
    if(N.overlay) { lv_obj_del(N.overlay); N.overlay = NULL; }
    N.ssid_dd = N.psk_ta = N.cc_ta = N.status_lbl = N.kb = NULL;
}
static void back_cb(lv_event_t *e) { (void)e; net_close(); }

/* ---- small style helpers ---- */
static lv_obj_t *mk_label(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t c) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}
static void style_btn(lv_obj_t *b, lv_obj_t *lbl, int primary) {
    lv_obj_set_style_bg_color(b, lv_color_hex(primary ? PK_PANEL : PK_PANEL2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(primary ? PK_TEAL : PK_LINE), 0);
    lv_obj_set_style_border_width(b, primary ? 2 : 1, 0);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    if(lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(primary ? PK_CREAM : PK_DIM), 0);
}

void prp_net_ui_show(int screen_w, int screen_h, int scale_pct, const char *mock_ssids) {
    if(N.overlay) return;
    N.w = screen_w; N.h = screen_h; N.scale = clampi(scale_pct, 50, 200);
    N.mock_ssids = mock_ssids;
    const int margin = clampi((N.h / 30) * N.scale / 100, 14, 64);
    const int gap = clampi((N.h / 56) * N.scale / 100, 8, 28);
    int large = (N.h >= 1400 || N.w >= 800 || N.scale >= 125);
    N.f_title = large ? &pk_serif_44 : &pk_serif_30;
    N.f_body = large ? &pk_mono_20 : &pk_mono_16;
    N.f_small = &pk_mono_16;

    lv_obj_t *scr = lv_scr_act();
    N.overlay = lv_obj_create(scr);
    lv_obj_set_size(N.overlay, N.w, N.h);
    lv_obj_center(N.overlay);
    lv_obj_set_style_bg_color(N.overlay, lv_color_hex(PK_BG), 0);
    lv_obj_set_style_bg_grad_color(N.overlay, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_grad_dir(N.overlay, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(N.overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(N.overlay, 0, 0);
    lv_obj_set_style_radius(N.overlay, 0, 0);
    lv_obj_set_style_pad_all(N.overlay, margin, 0);
    lv_obj_set_style_pad_row(N.overlay, gap, 0);
    lv_obj_set_flex_flow(N.overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(N.overlay, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *eb = mk_label(N.overlay, "RECOVERY · NETWORK", N.f_small, PK_TEAL);
    lv_obj_set_style_text_letter_space(eb, 2, 0);
    mk_label(N.overlay, "Connect to Wi-Fi", N.f_title, PK_CREAM);

    lv_obj_t *scan_btn = lv_btn_create(N.overlay);
    lv_obj_set_width(scan_btn, lv_pct(100));
    lv_obj_set_height(scan_btn, clampi(N.h / 16, 52, 110));
    N.scan_btn_lbl = mk_label(scan_btn, "Scan for networks", N.f_body, PK_CREAM);
    lv_obj_center(N.scan_btn_lbl);
    style_btn(scan_btn, N.scan_btn_lbl, 0);
    lv_obj_add_event_cb(scan_btn, scan_btn_cb, LV_EVENT_CLICKED, NULL);

    N.ssid_dd = lv_dropdown_create(N.overlay);
    lv_dropdown_set_options(N.ssid_dd, "(scan to list networks)");
    lv_obj_set_width(N.ssid_dd, lv_pct(100));
    lv_obj_set_style_text_font(N.ssid_dd, N.f_body, 0);

    N.psk_ta = lv_textarea_create(N.overlay);
    lv_textarea_set_one_line(N.psk_ta, true);
    lv_textarea_set_password_mode(N.psk_ta, true);
    lv_textarea_set_placeholder_text(N.psk_ta, "Wi-Fi password");
    lv_obj_set_width(N.psk_ta, lv_pct(100));
    lv_obj_set_style_text_font(N.psk_ta, N.f_body, 0);
    lv_obj_add_event_cb(N.psk_ta, ta_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(N.psk_ta, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);

    /* Region/country field (ISO-3166 alpha-2). Hidden until a connect reports
     * need-country — 5GHz/DFS channels can't transmit without a regulatory
     * domain, so the radio associates but DHCP never completes. Then we reveal
     * this and ask, instead of mislabelling it a bad password. */
    N.cc_ta = lv_textarea_create(N.overlay);
    lv_textarea_set_one_line(N.cc_ta, true);
    lv_textarea_set_max_length(N.cc_ta, 2);
    lv_textarea_set_placeholder_text(N.cc_ta, "Region code (e.g. US) — needed for 5 GHz");
    lv_obj_set_width(N.cc_ta, lv_pct(100));
    lv_obj_set_style_text_font(N.cc_ta, N.f_body, 0);
    lv_obj_add_event_cb(N.cc_ta, ta_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(N.cc_ta, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
    lv_obj_add_flag(N.cc_ta, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *conn_btn = lv_btn_create(N.overlay);
    lv_obj_set_width(conn_btn, lv_pct(100));
    lv_obj_set_height(conn_btn, clampi(N.h / 14, 60, 130));
    lv_obj_t *cl = mk_label(conn_btn, "Connect", N.f_body, PK_CREAM);
    lv_obj_center(cl);
    style_btn(conn_btn, cl, 1);
    lv_obj_add_event_cb(conn_btn, connect_btn_cb, LV_EVENT_CLICKED, NULL);

    N.status_lbl = mk_label(N.overlay, "", N.f_small, PK_DIM);
    lv_obj_set_width(N.status_lbl, lv_pct(100));
    lv_label_set_long_mode(N.status_lbl, LV_LABEL_LONG_WRAP);
    set_status(N.connected ? prp_net_ui_status() : "Not connected");

    lv_obj_t *back = lv_btn_create(N.overlay);
    lv_obj_set_width(back, lv_pct(100));
    lv_obj_set_height(back, clampi(N.h / 16, 52, 110));
    lv_obj_t *bl = mk_label(back, "Back", N.f_body, PK_DIM);
    lv_obj_center(bl);
    style_btn(back, bl, 0);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    /* Shared on-screen keyboard, floats over the content until a field focuses. */
    N.kb = lv_keyboard_create(N.overlay);
    lv_obj_add_flag(N.kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(N.kb, N.w, clampi(N.h * 2 / 5, 180, 520));
    lv_obj_align(N.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(N.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(N.kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(N.kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    /* Kick off a scan immediately on open. */
    do_scan();
}
