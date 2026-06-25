// PRP install wizard (see prp_wizard.h). The Progress step runs the real
// prp-install backend on device (forks it, parses its STEP/PROGRESS/LOG/DONE/
// ERROR line protocol over a pipe); with no prp-install present (the SDL sim) it
// falls back to a timer-driven mock so the flow stays exercisable. Styling from
// prp_theme.h; type from the pk_* fonts.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "prp_theme.h"
#include "prp_wizard.h"
#include "prp_net_ui.h"

LV_FONT_DECLARE(pk_serif_30);
LV_FONT_DECLARE(pk_serif_44);
LV_FONT_DECLARE(pk_mono_16);
LV_FONT_DECLARE(pk_mono_20);

enum { ST_WELCOME, ST_NETWORK, ST_OPTIONS, ST_CONFIRM, ST_PROGRESS, ST_DONE, ST_COUNT };

static struct {
    prp_wizard_cfg_t cfg;
    int step;
    int margin, gap;
    const lv_font_t *f_title, *f_body, *f_small;

    lv_obj_t *root, *title, *stepind, *content, *footer, *back, *back_lbl, *next, *next_lbl, *kb;

    // live widgets for the current step (read on Next, then content is cleaned)
    lv_obj_t *dd_flavor, *dd_init, *dd_desktop, *dd_dm, *dd_disk, *dd_ssid;
    lv_obj_t *ta_user, *ta_pass, *ta_host, *ta_psk;
    lv_obj_t *bar, *log;

    // captured choices
    char flavor[32], init_s[32], desktop[32], dm[32], disk[64];
    char user[64], pass[64], host[64], wifi[64];
    bool net_ok;

    lv_timer_t *prog_timer;
    int prog_i;
    char log_buf[2048];

    // real install (device): forked prp-install + its stdout pipe
    int install_fd;
    pid_t install_pid;
    char ibuf[512];
    size_t ilen;
} W;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void render_step(void);
static void wizard_close(void) {
    if(W.prog_timer) { lv_timer_del(W.prog_timer); W.prog_timer = NULL; }
    if(W.install_fd >= 0) { close(W.install_fd); W.install_fd = -1; }
    if(W.install_pid > 0) {
        int st;
        kill(W.install_pid, SIGTERM);
        waitpid(W.install_pid, &st, 0);
        W.install_pid = 0;
    }
    if(W.root) { lv_obj_del(W.root); W.root = NULL; }
}

/* ---- small style helpers ---- */
static void style_card(lv_obj_t *o) {
    lv_obj_set_style_bg_color(o, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(PK_LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 12, 0);
}
static void style_btn(lv_obj_t *btn, lv_obj_t *lbl, bool primary) {
    lv_obj_set_style_bg_color(btn, lv_color_hex(primary ? PK_PANEL : PK_PANEL2), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(primary ? PK_TEAL : PK_LINE), 0);
    lv_obj_set_style_border_width(btn, primary ? 2 : 1, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    if(lbl) lv_obj_set_style_text_color(lbl, lv_color_hex(primary ? PK_CREAM : PK_DIM), 0);
}
static lv_obj_t *mk_label(lv_obj_t *p, const char *txt, const lv_font_t *f, uint32_t color) {
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}
static lv_obj_t *mk_kicker(lv_obj_t *p, const char *txt) {
    lv_obj_t *l = mk_label(p, txt, W.f_small, PK_TEAL);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    return l;
}

/* ---- keyboard wiring ---- */
static void ta_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    if(code == LV_EVENT_FOCUSED) {
        lv_keyboard_set_textarea(W.kb, ta);
        lv_obj_clear_flag(W.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(W.kb);
    } else if(code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(W.kb, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(W.kb, NULL);
    }
}
static void kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(W.kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *ta = lv_keyboard_get_textarea(W.kb);
        if(ta) lv_obj_clear_state(ta, LV_STATE_FOCUSED);
    }
}

/* A labelled dropdown field inside the scrolling content. */
static lv_obj_t *mk_dropdown(const char *label, const char *opts) {
    lv_obj_t *wrap = lv_obj_create(W.content);
    lv_obj_set_width(wrap, lv_pct(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_pad_row(wrap, 4, 0);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    mk_kicker(wrap, label);
    lv_obj_t *dd = lv_dropdown_create(wrap);
    lv_dropdown_set_options(dd, opts && *opts ? opts : "—");
    lv_obj_set_width(dd, lv_pct(100));
    lv_obj_set_style_text_font(dd, W.f_body, 0);
    return dd;
}
/* A labelled text field. */
static lv_obj_t *mk_textfield(const char *label, const char *placeholder, bool password) {
    lv_obj_t *wrap = lv_obj_create(W.content);
    lv_obj_set_width(wrap, lv_pct(100));
    lv_obj_set_height(wrap, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrap, 0, 0);
    lv_obj_set_style_pad_all(wrap, 0, 0);
    lv_obj_set_style_pad_row(wrap, 4, 0);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
    mk_kicker(wrap, label);
    lv_obj_t *ta = lv_textarea_create(wrap);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, W.f_body, 0);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_DEFOCUSED, NULL);
    return ta;
}

/* ---- steps ---- */
static void render_welcome(void) {
    char line[160];
    mk_kicker(W.content, "ON-DEVICE INSTALL");
    mk_label(W.content, "Install PeacockOS", W.f_title, PK_CREAM);
    snprintf(line, sizeof(line), "This will install a fresh PeacockOS system on your %s, "
             "straight to its storage. It downloads everything it needs over the network, "
             "so no computer is required.", W.cfg.device_name ? W.cfg.device_name : "device");
    lv_obj_t *b = mk_label(W.content, line, W.f_small, PK_DIM);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_t *warn = mk_label(W.content,
        "Heads up: installing replaces what's on the target partition. Your call.", W.f_small, PK_VIOLET);
    lv_obj_set_width(warn, lv_pct(100));
    lv_label_set_long_mode(warn, LV_LABEL_LONG_WRAP);
}

static void net_setup_btn_cb(lv_event_t *e) {
    (void)e;
    prp_net_ui_show(W.cfg.screen_w, W.cfg.screen_h, W.cfg.scale_pct, W.cfg.wifi_ssids);
}

static void render_network(void) {
    mk_kicker(W.content, "STEP · NETWORK");
    mk_label(W.content, "Get online", W.f_title, PK_CREAM);
    lv_obj_t *b = mk_label(W.content,
        "PeacockOS is downloaded over the network. Connect to Wi-Fi here, or just continue "
        "if you're already on wired/USB.",
        W.f_small, PK_DIM);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn = lv_btn_create(W.content);
    lv_obj_set_width(btn, lv_pct(100));
    lv_obj_set_height(btn, clampi(W.cfg.screen_h / 14, 60, 130));
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_PANEL), 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(PK_TEAL), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(PK_TEALDK), LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, net_setup_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(btn, "Connect to Wi-Fi", W.f_body, PK_CREAM);
    lv_obj_center(bl);

    int ok = prp_net_ui_connected();
    lv_obj_t *status = mk_label(W.content, prp_net_ui_status(), W.f_small, ok ? PK_TEAL : PK_DIM);
    lv_obj_set_width(status, lv_pct(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
}

static void render_options(void) {
    mk_kicker(W.content, "STEP · OPTIONS");
    mk_label(W.content, "Set it up", W.f_title, PK_CREAM);

    W.dd_flavor  = mk_dropdown("BASE DISTRO", W.cfg.flavors);
    W.dd_init    = mk_dropdown("INIT SYSTEM", W.cfg.inits);
    W.dd_desktop = mk_dropdown("DESKTOP", W.cfg.desktops);
    W.dd_dm      = mk_dropdown("LOGIN MANAGER", W.cfg.dms);
    W.ta_user    = mk_textfield("USERNAME", "e.g. emre", false);
    W.ta_pass    = mk_textfield("PASSWORD", "Account password", true);
    W.ta_host    = mk_textfield("HOSTNAME", "e.g. peacock", false);
    W.dd_disk    = mk_dropdown("INSTALL TO", W.cfg.disks);
}

static void summary_row(const char *k, const char *v) {
    lv_obj_t *row = lv_obj_create(W.content);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    style_card(row);
    lv_obj_set_style_pad_all(row, clampi(W.gap, 10, 16), 0);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *kl = mk_kicker(row, k);
    lv_obj_set_width(kl, lv_pct(42));
    lv_obj_t *vl = mk_label(row, (v && *v) ? v : "—", W.f_body, PK_CREAM);
    lv_obj_set_flex_grow(vl, 1);
    lv_label_set_long_mode(vl, LV_LABEL_LONG_DOT);
}

static void render_confirm(void) {
    char net[96];
    mk_kicker(W.content, "STEP · CONFIRM");
    mk_label(W.content, "Ready to install", W.f_title, PK_CREAM);
    summary_row("DEVICE", W.cfg.device_name);
    snprintf(net, sizeof(net), W.net_ok ? "Wi-Fi: %s" : "%s", W.net_ok ? W.wifi : "Wired / continue");
    summary_row("NETWORK", net);
    summary_row("DISTRO", W.flavor);
    summary_row("INIT", W.init_s);
    summary_row("DESKTOP", W.desktop);
    summary_row("LOGIN", W.dm);
    summary_row("USER", W.user);
    summary_row("HOSTNAME", W.host);
    summary_row("INSTALL TO", W.disk);
}

static const char *k_prog_steps[] = {
    "Connecting to genmirror.peacockos.org",
    "Preparing the target partition",
    "Resolving packages",
    "Downloading packages",
    "Verifying signatures",
    "Installing the system",
    "Installing the bootloader",
    "Configuring user and services",
    "Finishing up",
};

static void prog_timer_cb(lv_timer_t *t) {
    const int n = (int)(sizeof(k_prog_steps) / sizeof(k_prog_steps[0]));
    if(W.prog_i >= n) {
        lv_timer_del(t);
        W.prog_timer = NULL;
        W.step = ST_DONE;
        render_step();
        return;
    }
    if(W.bar) lv_bar_set_value(W.bar, (W.prog_i + 1) * 100 / n, LV_ANIM_ON);
    size_t len = strlen(W.log_buf);
    snprintf(W.log_buf + len, sizeof(W.log_buf) - len, "%s%s", len ? "\n" : "", k_prog_steps[W.prog_i]);
    if(W.log) {
        lv_label_set_text(W.log, W.log_buf);
        lv_obj_t *cont = lv_obj_get_parent(W.log);
        if(cont) lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
    W.prog_i++;
}

/* Append one line to the progress log view. */
static void wlog(const char *line) {
    size_t len = strlen(W.log_buf);
    snprintf(W.log_buf + len, sizeof(W.log_buf) - len, "%s%s", len ? "\n" : "", line);
    if(W.log) {
        lv_label_set_text(W.log, W.log_buf);
        lv_obj_t *cont = lv_obj_get_parent(W.log);
        if(cont) lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static int prp_install_present(void) {
    return access("/usr/bin/prp-install", X_OK) == 0 || access("/sbin/prp-install", X_OK) == 0;
}

static void done_close_cb(lv_event_t *e);  /* defined with render_done below */

static void install_finish(int ok) {
    if(W.install_fd >= 0) { close(W.install_fd); W.install_fd = -1; }
    if(W.install_pid > 0) { int st; (void)waitpid(W.install_pid, &st, WNOHANG); W.install_pid = 0; }
    if(W.prog_timer) { lv_timer_del(W.prog_timer); W.prog_timer = NULL; }
    if(ok) { W.step = ST_DONE; render_step(); return; }
    /* Failure: the footer is hidden during PROGRESS, so the user would be stuck.
     * Offer a way back to the recovery menu (nothing past the logged steps ran). */
    wlog("");
    wlog("Install failed. You can return to recovery and try again.");
    lv_obj_t *back = lv_btn_create(W.content);
    lv_obj_set_width(back, lv_pct(80));
    lv_obj_set_height(back, clampi(W.cfg.screen_h / 14, 56, 120));
    lv_obj_t *bl = mk_label(back, "Back to recovery", W.f_body, PK_CREAM);
    lv_obj_center(bl);
    style_btn(back, bl, true);
    lv_obj_add_event_cb(back, done_close_cb, LV_EVENT_CLICKED, NULL);
}

/* Parse one line of prp-install's STEP/PROGRESS/LOG/DONE/ERROR protocol. */
static void parse_install_line(char *line) {
    if(strncmp(line, "PROGRESS ", 9) == 0) {
        if(W.bar) lv_bar_set_value(W.bar, atoi(line + 9), LV_ANIM_ON);
    } else if(strncmp(line, "STEP ", 5) == 0) {
        char *t = line + 5;  /* skip the "<i> <total> " tokens */
        for(int k = 0; k < 2; k++) { char *sp = strchr(t, ' '); if(sp) t = sp + 1; }
        char buf[160]; snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_RIGHT, t);
        wlog(buf);
    } else if(strncmp(line, "LOG ", 4) == 0) {
        char buf[200]; snprintf(buf, sizeof(buf), "   %s", line + 4);
        wlog(buf);
    } else if(strcmp(line, "DONE") == 0) {
        if(W.bar) lv_bar_set_value(W.bar, 100, LV_ANIM_ON);
        install_finish(1);
    } else if(strncmp(line, "ERROR ", 6) == 0) {
        char buf[220]; snprintf(buf, sizeof(buf), "%s %s", LV_SYMBOL_WARNING, line + 6);
        wlog(buf);
        install_finish(0);  /* stay on the progress page showing the failure */
    }
}

static void install_poll(lv_timer_t *t) {
    (void)t;
    if(W.install_fd < 0) return;
    char rd[256];
    ssize_t n = read(W.install_fd, rd, sizeof(rd));
    if(n > 0) {
        for(ssize_t i = 0; i < n; i++) {
            char c = rd[i];
            if(c == '\n' || W.ilen + 1 >= sizeof(W.ibuf)) {
                W.ibuf[W.ilen] = '\0';
                if(W.ibuf[0]) parse_install_line(W.ibuf);
                W.ilen = 0;
                if(W.install_fd < 0) return;  /* parse() may have finished us */
            } else {
                W.ibuf[W.ilen++] = c;
            }
        }
    } else if(n == 0) {
        wlog("Installer exited.");
        install_finish(0);
    }
    /* n < 0 (EAGAIN): no data this tick — poll again next tick. */
}

/* Fork prp-install with the captured choices; parse its pipe in install_poll. */
static void start_real_install(void) {
    int fds[2];
    if(pipe(fds) != 0) { wlog("FAILED: pipe()"); return; }
    pid_t pid = fork();
    if(pid < 0) { close(fds[0]); close(fds[1]); wlog("FAILED: fork()"); return; }
    if(pid == 0) {
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        execlp("prp-install", "prp-install",
               "--flavor", W.flavor, "--init", W.init_s, "--desktop", W.desktop,
               "--dm", W.dm, "--user", W.user, "--pass", W.pass,
               "--host", W.host, "--disk", W.disk, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    W.install_fd = fds[0];
    W.install_pid = pid;
    W.ilen = 0;
    W.prog_timer = lv_timer_create(install_poll, 150, NULL);
}

static void render_progress(void) {
    mk_kicker(W.content, "INSTALLING");
    mk_label(W.content, "Hang tight", W.f_title, PK_CREAM);

    W.bar = lv_bar_create(W.content);
    lv_obj_set_width(W.bar, lv_pct(100));
    lv_obj_set_height(W.bar, 12);
    lv_obj_set_style_radius(W.bar, 6, 0);
    lv_obj_set_style_bg_color(W.bar, lv_color_hex(PK_PANEL2), LV_PART_MAIN);
    lv_obj_set_style_bg_color(W.bar, lv_color_hex(PK_TEAL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(W.bar, lv_color_hex(PK_VIOLET), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(W.bar, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);
    lv_bar_set_value(W.bar, 0, LV_ANIM_OFF);

    lv_obj_t *box = lv_obj_create(W.content);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_flex_grow(box, 1);
    style_card(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(PK_PANEL2), 0);
    lv_obj_set_style_pad_all(box, 12, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    W.log = lv_label_create(box);
    lv_obj_set_width(W.log, lv_pct(100));
    lv_label_set_long_mode(W.log, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(W.log, W.f_small, 0);
    lv_obj_set_style_text_color(W.log, lv_color_hex(PK_CREAM), 0);
    lv_label_set_text(W.log, "");

    W.prog_i = 0;
    W.log_buf[0] = '\0';
    W.install_fd = -1;
    W.install_pid = 0;
    if(prp_install_present()) {
        wlog("Starting installer…");
        start_real_install();          // device: real prp-install + protocol parse
    } else {
        W.prog_timer = lv_timer_create(prog_timer_cb, 650, NULL);  // sim: mock
    }
}

static void done_close_cb(lv_event_t *e) { (void)e; wizard_close(); }

static void render_done(void) {
    lv_obj_set_flex_align(W.content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_kicker(W.content, "DONE");
    lv_obj_t *t = mk_label(W.content, "PeacockOS installed", W.f_title, PK_CREAM);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *b = mk_label(W.content,
        "Reboot to start your new system. If something's off, you can always come back to recovery.",
        W.f_small, PK_DIM);
    lv_obj_set_width(b, lv_pct(85));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *reboot = lv_btn_create(W.content);
    lv_obj_set_width(reboot, lv_pct(70));
    lv_obj_set_height(reboot, clampi(W.cfg.screen_h / 14, 60, 130));
    lv_obj_t *rl = mk_label(reboot, "Reboot now", W.f_body, PK_CREAM);
    lv_obj_center(rl);
    style_btn(reboot, rl, true);
    lv_obj_add_event_cb(reboot, done_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_btn_create(W.content);
    lv_obj_set_width(back, lv_pct(70));
    lv_obj_set_height(back, clampi(W.cfg.screen_h / 16, 52, 110));
    lv_obj_t *bl = mk_label(back, "Back to recovery", W.f_body, PK_DIM);
    lv_obj_center(bl);
    style_btn(back, bl, false);
    lv_obj_add_event_cb(back, done_close_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- navigation ---- */
static void capture_step(void) {
    if(W.step == ST_NETWORK) {
        // The shared Wi-Fi overlay (prp_net_ui) tracks the real connection state.
        W.net_ok = prp_net_ui_connected();
        snprintf(W.wifi, sizeof(W.wifi), "%s", prp_net_ui_status());
    } else if(W.step == ST_OPTIONS) {
        if(W.dd_flavor) lv_dropdown_get_selected_str(W.dd_flavor, W.flavor, sizeof(W.flavor));
        if(W.dd_init) lv_dropdown_get_selected_str(W.dd_init, W.init_s, sizeof(W.init_s));
        if(W.dd_desktop) lv_dropdown_get_selected_str(W.dd_desktop, W.desktop, sizeof(W.desktop));
        if(W.dd_dm) lv_dropdown_get_selected_str(W.dd_dm, W.dm, sizeof(W.dm));
        if(W.dd_disk) lv_dropdown_get_selected_str(W.dd_disk, W.disk, sizeof(W.disk));
        if(W.ta_user) snprintf(W.user, sizeof(W.user), "%s", lv_textarea_get_text(W.ta_user));
        if(W.ta_pass) snprintf(W.pass, sizeof(W.pass), "%s", lv_textarea_get_text(W.ta_pass));
        if(W.ta_host) snprintf(W.host, sizeof(W.host), "%s", lv_textarea_get_text(W.ta_host));
    }
}

static void next_cb(lv_event_t *e) {
    (void)e;
    capture_step();
    if(W.step == ST_CONFIRM) { W.step = ST_PROGRESS; render_step(); return; }
    if(W.step < ST_DONE) { W.step++; render_step(); }
}
static void back_cb(lv_event_t *e) {
    (void)e;
    if(W.step > ST_WELCOME) { W.step--; render_step(); }
}

static void set_footer(bool show_back, const char *next_txt, bool footer_visible) {
    if(footer_visible) lv_obj_clear_flag(W.footer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(W.footer, LV_OBJ_FLAG_HIDDEN);
    if(show_back) lv_obj_clear_flag(W.back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(W.back, LV_OBJ_FLAG_HIDDEN);
    if(next_txt) lv_label_set_text(W.next_lbl, next_txt);
}

static void render_step(void) {
    // reset content container defaults each render (done step re-centers it)
    lv_obj_clean(W.content);
    lv_obj_set_flex_align(W.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(W.content, LV_DIR_VER);

    char ind[24];
    snprintf(ind, sizeof(ind), "STEP %d / %d", W.step + 1, ST_DONE);
    lv_label_set_text(W.stepind, ind);

    switch(W.step) {
        case ST_WELCOME:  lv_label_set_text(W.title, "Welcome");  render_welcome();  set_footer(false, "Get started", true); break;
        case ST_NETWORK:  lv_label_set_text(W.title, "Network");  render_network();  set_footer(true, "Continue", true); break;
        case ST_OPTIONS:  lv_label_set_text(W.title, "Options");  render_options();  set_footer(true, "Review", true); break;
        case ST_CONFIRM:  lv_label_set_text(W.title, "Confirm");  render_confirm();  set_footer(true, "Install", true); break;
        case ST_PROGRESS: lv_label_set_text(W.title, "Installing"); render_progress(); set_footer(false, "", false); break;
        case ST_DONE:     lv_label_set_text(W.title, "Done");     render_done();     set_footer(false, "", false); break;
        default: break;
    }
    // primary accent on the Confirm step's Install button
    style_btn(W.next, W.next_lbl, W.step == ST_CONFIRM);
}

void prp_wizard_show(const prp_wizard_cfg_t *cfg) {
    if(W.root) return;
    memset(&W, 0, sizeof(W));
    W.cfg = *cfg;
    const int w = cfg->screen_w, h = cfg->screen_h;
    const int scale = clampi(cfg->scale_pct, 50, 200);
    W.margin = clampi((h / 36) * scale / 100, 12, 64);
    W.gap = clampi((h / 64) * scale / 100, 8, 32);
    bool large = (h >= 1400 || w >= 800 || scale >= 125);
    W.f_title = large ? &pk_serif_44 : &pk_serif_30;
    W.f_body = large ? &pk_mono_20 : &pk_mono_16;
    W.f_small = &pk_mono_16;

    lv_obj_t *scr = lv_scr_act();
    W.root = lv_obj_create(scr);
    lv_obj_set_size(W.root, w, h);
    lv_obj_center(W.root);
    lv_obj_set_style_bg_color(W.root, lv_color_hex(PK_BG), 0);
    lv_obj_set_style_bg_grad_color(W.root, lv_color_hex(0x0A1018), 0);
    lv_obj_set_style_bg_grad_dir(W.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(W.root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(W.root, 0, 0);
    lv_obj_set_style_radius(W.root, 0, 0);
    lv_obj_set_style_pad_all(W.root, 0, 0);
    lv_obj_set_flex_flow(W.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(W.root, LV_OBJ_FLAG_SCROLLABLE);

    // header
    lv_obj_t *header = lv_obj_create(W.root);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, W.margin, 0);
    lv_obj_set_style_pad_bottom(header, W.gap, 0);
    lv_obj_set_style_pad_row(header, 2, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);
    W.stepind = mk_kicker(header, "STEP 1 / 5");
    W.title = mk_label(header, "Welcome", W.f_title, PK_CREAM);

    // content (scrolls)
    W.content = lv_obj_create(W.root);
    lv_obj_set_width(W.content, lv_pct(100));
    lv_obj_set_flex_grow(W.content, 1);
    lv_obj_set_style_bg_opa(W.content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(W.content, 0, 0);
    lv_obj_set_style_pad_left(W.content, W.margin, 0);
    lv_obj_set_style_pad_right(W.content, W.margin, 0);
    lv_obj_set_style_pad_row(W.content, W.gap, 0);
    lv_obj_set_flex_flow(W.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(W.content, LV_SCROLLBAR_MODE_OFF);

    // footer
    W.footer = lv_obj_create(W.root);
    lv_obj_set_width(W.footer, lv_pct(100));
    lv_obj_set_height(W.footer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(W.footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(W.footer, 0, 0);
    lv_obj_set_style_pad_all(W.footer, W.margin, 0);
    lv_obj_set_style_pad_column(W.footer, W.gap, 0);
    lv_obj_set_flex_flow(W.footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(W.footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(W.footer, LV_OBJ_FLAG_SCROLLABLE);

    const int bh = clampi(h / 15, 56, 120);
    W.back = lv_btn_create(W.footer);
    lv_obj_set_height(W.back, bh);
    lv_obj_set_flex_grow(W.back, 1);
    W.back_lbl = mk_label(W.back, "Back", W.f_body, PK_DIM);
    lv_obj_center(W.back_lbl);
    style_btn(W.back, W.back_lbl, false);
    lv_obj_add_event_cb(W.back, back_cb, LV_EVENT_CLICKED, NULL);

    W.next = lv_btn_create(W.footer);
    lv_obj_set_height(W.next, bh);
    lv_obj_set_flex_grow(W.next, 2);
    W.next_lbl = mk_label(W.next, "Get started", W.f_body, PK_CREAM);
    lv_obj_center(W.next_lbl);
    style_btn(W.next, W.next_lbl, true);
    lv_obj_add_event_cb(W.next, next_cb, LV_EVENT_CLICKED, NULL);

    // shared on-screen keyboard (floats over the content, hidden until a field is focused)
    W.kb = lv_keyboard_create(W.root);
    lv_obj_add_flag(W.kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(W.kb, w, clampi(h * 2 / 5, 180, 520));
    lv_obj_align(W.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(W.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(W.kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(W.kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    W.step = ST_WELCOME;
    render_step();
}
