// PeacockOS first-boot OOBE wizard (see oobe_wizard.h). Chrome/styling shared in spirit with the
// PRP install wizard; the middle pages render one flavor configure.toml oobe-stage each, and the
// Progress page applies the blueprint by forking `peacock-oobe --apply` and parsing its
// STEP/PROGRESS/LOG/DONE/ERROR protocol. SDL sim (no /sbin/peacock-oobe) mocks the apply.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "prp_theme.h"
#include "oobe_wizard.h"
#include "blueprint.h"

LV_FONT_DECLARE(pk_serif_30);
LV_FONT_DECLARE(pk_serif_44);
LV_FONT_DECLARE(pk_mono_16);
LV_FONT_DECLARE(pk_mono_20);

static struct {
    oobe_cfg_t cfg;
    int page, n_stages;       // pages: 0=welcome, 1..n=stage, n+1=confirm, n+2=progress, n+3=done
    int margin, gap;
    const lv_font_t *f_title, *f_body, *f_small;

    lv_obj_t *root, *title, *stepind, *content, *footer, *back, *back_lbl, *next, *next_lbl, *kb;

    bp_blueprint *bp;
    bp_answers *ans;
    const bp_stage *ord[64];

    // widgets of the current stage page, for capture
    lv_obj_t *bp_w[24];
    const bp_field *bp_f[24];
    int n_bp_w;

    // captured passwords — applied via --secret, never written to the answers store
    char sec_k[8][32];
    char sec_v[8][160];
    int n_sec;

    lv_obj_t *bar, *log;
    char log_buf[2048];
    lv_timer_t *prog_timer;
    int prog_i;

    int apply_fd;
    pid_t apply_pid;
    char ibuf[512];
    size_t ilen;
} W;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int PAGE_CONFIRM(void) { return W.n_stages + 1; }
static int PAGE_PROGRESS(void) { return W.n_stages + 2; }
static int PAGE_DONE(void) { return W.n_stages + 3; }

static void render_page(void);

static void wizard_close(void) {
    if(W.prog_timer) { lv_timer_del(W.prog_timer); W.prog_timer = NULL; }
    if(W.apply_fd >= 0) { close(W.apply_fd); W.apply_fd = -1; }
    if(W.apply_pid > 0) { int st; kill(W.apply_pid, SIGTERM); waitpid(W.apply_pid, &st, 0); W.apply_pid = 0; }
    if(W.root) { lv_obj_del(W.root); W.root = NULL; }
    if(W.bp) { bp_free(W.bp); W.bp = NULL; }
    if(W.ans) { bp_answers_free(W.ans); W.ans = NULL; }
}

/* ---- style helpers (mirrors prp_wizard.c) ---- */
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

/* ---- polymorphic blueprint fields (from P1) ---- */
static void dropdown_select_in(lv_obj_t *dd, const char *opts, const char *val) {
    if(!dd || !opts || !val || !*val) return;
    int idx = 0;
    for(const char *p = opts; p; ) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if(strlen(val) == len && strncmp(p, val, len) == 0) { lv_dropdown_set_selected(dd, idx); return; }
        idx++;
        p = nl ? nl + 1 : NULL;
    }
}
static void render_stage_fields(const bp_stage *st) {
    W.n_bp_w = 0;
    for(size_t i = 0; i < st->n_fields; i++) {
        const bp_field *f = &st->fields[i];
        if(!bp_when_eval(f->when, W.ans)) continue;
        const char *cur = f->key ? bp_answers_get(W.ans, f->key) : NULL;
        char *defv = bp_expand(f->def ? f->def : "", W.ans);
        const char *initv = (cur && *cur) ? cur : defv;
        lv_obj_t *w = NULL;
        switch(f->type) {
            case BP_FIELD_DROPDOWN:
                w = mk_dropdown(f->label, f->options);
                dropdown_select_in(w, f->options, initv);
                break;
            case BP_FIELD_TOGGLE:
                w = mk_dropdown(f->label, "false\ntrue");
                dropdown_select_in(w, "false\ntrue", initv);
                break;
            case BP_FIELD_PASSWORD:
                w = mk_textfield(f->label, f->placeholder ? f->placeholder : "", true);
                break;
            case BP_FIELD_TEXT:
                w = mk_textfield(f->label, f->placeholder ? f->placeholder : "", false);
                if(initv && *initv) lv_textarea_set_text(w, initv);
                break;
            case BP_FIELD_INFO:
                mk_label(W.content, f->label, W.f_body, PK_DIM);
                break;
        }
        free(defv);
        if(w && f->key && W.n_bp_w < 24) { W.bp_w[W.n_bp_w] = w; W.bp_f[W.n_bp_w] = f; W.n_bp_w++; }
    }
}
/* Returns 0 to block advancing (validation failed). Passwords go to secrets, not the store. */
static int capture_stage_fields(void) {
    for(int i = 0; i < W.n_bp_w; i++) {
        const bp_field *f = W.bp_f[i];
        char val[256] = "";
        if(f->type == BP_FIELD_DROPDOWN || f->type == BP_FIELD_TOGGLE)
            lv_dropdown_get_selected_str(W.bp_w[i], val, sizeof val);
        else
            snprintf(val, sizeof val, "%s", lv_textarea_get_text(W.bp_w[i]));
        if(f->required && !*val) return 0;
        if(!bp_validate(f->validate, val)) return 0;
        if(f->type == BP_FIELD_PASSWORD) {
            int found = -1;
            for(int s = 0; s < W.n_sec; s++) if(!strcmp(W.sec_k[s], f->key)) { found = s; break; }
            if(found < 0 && W.n_sec < 8) { found = W.n_sec++; snprintf(W.sec_k[found], sizeof W.sec_k[found], "%s", f->key); }
            if(found >= 0) snprintf(W.sec_v[found], sizeof W.sec_v[found], "%s", val);
        } else {
            bp_answers_set(W.ans, f->key, val);
        }
    }
    return 1;
}

/* ---- pages ---- */
static void render_welcome(void) {
    char line[200];
    mk_kicker(W.content, "FIRST-TIME SETUP");
    mk_label(W.content, "Welcome to PeacockOS", W.f_title, PK_CREAM);
    snprintf(line, sizeof line, "Let's set up your %s. A few quick questions and your %s desktop "
             "will be ready.", W.cfg.device_name ? W.cfg.device_name : "device",
             W.cfg.flavor ? W.cfg.flavor : "system");
    lv_obj_t *b = mk_label(W.content, line, W.f_small, PK_DIM);
    lv_obj_set_width(b, lv_pct(100));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
}

static void render_stage(int idx) {
    const bp_stage *st = W.ord[idx];
    char kick[32];
    snprintf(kick, sizeof kick, "STEP %d / %d", idx + 1, W.n_stages);
    mk_kicker(W.content, kick);
    mk_label(W.content, st->title ? st->title : st->id, W.f_title, PK_CREAM);
    if(st->description) {
        lv_obj_t *d = mk_label(W.content, st->description, W.f_small, PK_DIM);
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    }
    render_stage_fields(st);
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
    mk_kicker(W.content, "CONFIRM");
    mk_label(W.content, "Ready to finish setup", W.f_title, PK_CREAM);
    /* Show every captured answer (passwords are never in the store). */
    for(size_t i = 0; i < W.ans->n; i++) {
        char key[64];
        snprintf(key, sizeof key, "%s", W.ans->keys[i]);
        for(char *p = key; *p; p++) *p = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
        summary_row(key, W.ans->vals[i]);
    }
    if(W.n_sec) summary_row("PASSWORD", "•••••••• (set)");
}

/* ---- progress / apply ---- */
static void wlog(const char *line) {
    size_t len = strlen(W.log_buf);
    snprintf(W.log_buf + len, sizeof(W.log_buf) - len, "%s%s", len ? "\n" : "", line);
    if(W.log) {
        lv_label_set_text(W.log, W.log_buf);
        lv_obj_t *cont = lv_obj_get_parent(W.log);
        if(cont) lv_obj_scroll_to_y(cont, LV_COORD_MAX, LV_ANIM_OFF);
    }
}

static int peacock_oobe_present(void) { return access("/sbin/peacock-oobe", X_OK) == 0; }

static void apply_finish(int ok) {
    if(W.apply_fd >= 0) { close(W.apply_fd); W.apply_fd = -1; }
    if(W.apply_pid > 0) { int st; (void)waitpid(W.apply_pid, &st, WNOHANG); W.apply_pid = 0; }
    if(W.prog_timer) { lv_timer_del(W.prog_timer); W.prog_timer = NULL; }
    if(ok) { W.page = PAGE_DONE(); render_page(); return; }
    wlog("");
    wlog("Setup hit a snag. It will retry on the next boot.");
}

static void parse_apply_line(char *line) {
    if(strncmp(line, "PROGRESS ", 9) == 0) {
        if(W.bar) lv_bar_set_value(W.bar, atoi(line + 9), LV_ANIM_ON);
    } else if(strncmp(line, "STEP ", 5) == 0) {
        char buf[200]; snprintf(buf, sizeof buf, "%s %s", LV_SYMBOL_RIGHT, line + 5); wlog(buf);
    } else if(strncmp(line, "LOG ", 4) == 0) {
        char buf[220]; snprintf(buf, sizeof buf, "   %s", line + 4); wlog(buf);
    } else if(strcmp(line, "DONE") == 0) {
        if(W.bar) lv_bar_set_value(W.bar, 100, LV_ANIM_ON);
        apply_finish(1);
    } else if(strncmp(line, "ERROR ", 6) == 0) {
        char buf[240]; snprintf(buf, sizeof buf, "%s %s", LV_SYMBOL_WARNING, line + 6); wlog(buf);
        apply_finish(0);
    }
}

static void apply_poll(lv_timer_t *t) {
    (void)t;
    if(W.apply_fd < 0) return;
    char rd[256];
    ssize_t n = read(W.apply_fd, rd, sizeof rd);
    if(n > 0) {
        for(ssize_t i = 0; i < n; i++) {
            char c = rd[i];
            if(c == '\n' || W.ilen + 1 >= sizeof W.ibuf) {
                W.ibuf[W.ilen] = '\0';
                if(W.ibuf[0]) parse_apply_line(W.ibuf);
                W.ilen = 0;
                if(W.apply_fd < 0) return;
            } else {
                W.ibuf[W.ilen++] = c;
            }
        }
    } else if(n == 0) {
        wlog("Setup process exited.");
        apply_finish(0);
    }
}

static void start_real_apply(void) {
    bp_answers_save(W.ans, "/tmp/oobe-answers.toml");
    int fds[2];
    if(pipe(fds) != 0) { wlog("FAILED: pipe()"); return; }
    pid_t pid = fork();
    if(pid < 0) { close(fds[0]); close(fds[1]); wlog("FAILED: fork()"); return; }
    if(pid == 0) {
        dup2(fds[1], 1); dup2(fds[1], 2);
        close(fds[0]); close(fds[1]);
        char *argv[48]; int ac = 0;
        argv[ac++] = "peacock-oobe"; argv[ac++] = "--apply";
        argv[ac++] = "--kind"; argv[ac++] = "oobe";
        argv[ac++] = "--root"; argv[ac++] = (char *)(W.cfg.root ? W.cfg.root : "/");
        if(W.cfg.blueprint_local) {
            argv[ac++] = "--local"; argv[ac++] = (char *)W.cfg.blueprint_local;
        } else {
            argv[ac++] = "--base"; argv[ac++] = (char *)W.cfg.blueprint_base_url;
            argv[ac++] = "--pubkey"; argv[ac++] = (char *)W.cfg.blueprint_pubkey;
        }
        argv[ac++] = "--answers"; argv[ac++] = "/tmp/oobe-answers.toml";
        static char sbuf[8][200];
        for(int i = 0; i < W.n_sec && ac < 44; i++) {
            snprintf(sbuf[i], sizeof sbuf[i], "%s=%s", W.sec_k[i], W.sec_v[i]);
            argv[ac++] = "--secret"; argv[ac++] = sbuf[i];
        }
        argv[ac] = NULL;
        execvp("peacock-oobe", argv);
        _exit(127);
    }
    close(fds[1]);
    (void)fcntl(fds[0], F_SETFL, O_NONBLOCK);
    W.apply_fd = fds[0];
    W.apply_pid = pid;
    W.ilen = 0;
    W.prog_timer = lv_timer_create(apply_poll, 150, NULL);
}

static const char *k_mock[] = {
    "Creating your account", "Naming the device", "Setting timezone", "Setting locale",
    "Installing the desktop", "Finishing up",
};
static void mock_timer_cb(lv_timer_t *t) {
    const int n = (int)(sizeof k_mock / sizeof k_mock[0]);
    if(W.prog_i >= n) { lv_timer_del(t); W.prog_timer = NULL; W.page = PAGE_DONE(); render_page(); return; }
    if(W.bar) lv_bar_set_value(W.bar, (W.prog_i + 1) * 100 / n, LV_ANIM_ON);
    char buf[120]; snprintf(buf, sizeof buf, "%s %s", LV_SYMBOL_RIGHT, k_mock[W.prog_i]); wlog(buf);
    W.prog_i++;
}

static void render_progress(void) {
    mk_kicker(W.content, "SETTING UP");
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
    W.apply_fd = -1;
    W.apply_pid = 0;
    if(peacock_oobe_present()) {
        wlog("Applying your setup…");
        start_real_apply();
    } else {
        W.prog_timer = lv_timer_create(mock_timer_cb, 600, NULL);
    }
}

static void done_close_cb(lv_event_t *e) { (void)e; wizard_close(); exit(0); }

static void render_done(void) {
    lv_obj_set_flex_align(W.content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    mk_kicker(W.content, "ALL SET");
    lv_obj_t *t = mk_label(W.content, "You're ready to go", W.f_title, PK_CREAM);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *b = mk_label(W.content, "Your PeacockOS desktop is configured. Tap continue to start it.",
                           W.f_small, PK_DIM);
    lv_obj_set_width(b, lv_pct(85));
    lv_label_set_long_mode(b, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *go = lv_btn_create(W.content);
    lv_obj_set_width(go, lv_pct(70));
    lv_obj_set_height(go, clampi(W.cfg.screen_h / 14, 60, 130));
    lv_obj_t *gl = mk_label(go, "Continue", W.f_body, PK_CREAM);
    lv_obj_center(gl);
    style_btn(go, gl, true);
    lv_obj_add_event_cb(go, done_close_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- navigation ---- */
static int capture_page(void) {
    if(W.page >= 1 && W.page <= W.n_stages) return capture_stage_fields();
    return 1;
}
static void next_cb(lv_event_t *e) {
    (void)e;
    if(!capture_page()) return; /* validation failed — stay */
    if(W.page == PAGE_CONFIRM()) { W.page = PAGE_PROGRESS(); render_page(); return; }
    if(W.page < PAGE_DONE()) { W.page++; render_page(); }
}
static void back_cb(lv_event_t *e) {
    (void)e;
    if(W.page > 0) { W.page--; render_page(); }
}
static void set_footer(bool show_back, const char *next_txt, bool footer_visible) {
    if(footer_visible) lv_obj_clear_flag(W.footer, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(W.footer, LV_OBJ_FLAG_HIDDEN);
    if(show_back) lv_obj_clear_flag(W.back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(W.back, LV_OBJ_FLAG_HIDDEN);
    if(next_txt) lv_label_set_text(W.next_lbl, next_txt);
}

static void render_page(void) {
    lv_obj_clean(W.content);
    lv_obj_set_flex_align(W.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(W.content, LV_DIR_VER);

    char ind[24];
    snprintf(ind, sizeof ind, "%d / %d", W.page + 1, PAGE_DONE() + 1);
    lv_label_set_text(W.stepind, ind);

    if(W.page == 0) {
        lv_label_set_text(W.title, "Welcome"); render_welcome();
        set_footer(false, "Get started", true);
    } else if(W.page >= 1 && W.page <= W.n_stages) {
        lv_label_set_text(W.title, "Setup"); render_stage(W.page - 1);
        set_footer(true, "Next", true);
    } else if(W.page == PAGE_CONFIRM()) {
        lv_label_set_text(W.title, "Confirm"); render_confirm();
        set_footer(true, "Finish", true);
    } else if(W.page == PAGE_PROGRESS()) {
        lv_label_set_text(W.title, "Setting up"); render_progress();
        set_footer(false, "", false);
    } else {
        lv_label_set_text(W.title, "Done"); render_done();
        set_footer(false, "", false);
    }
    style_btn(W.next, W.next_lbl, W.page == PAGE_CONFIRM());
}

void prp_oobe_show(const oobe_cfg_t *cfg) {
    if(W.root) return;
    memset(&W, 0, sizeof W);
    W.cfg = *cfg;

    /* Load the configure.toml: a local copy (sim/offline) or fetched+verified from genmirror. */
    const char *bp_path = NULL;
    char localbuf[640];
    if(cfg->blueprint_local) {
        snprintf(localbuf, sizeof localbuf, "%s/configure.toml", cfg->blueprint_local);
        bp_path = localbuf;
    } else if(cfg->blueprint_base_url && cfg->blueprint_pubkey) {
        char url[700], err[256];
        snprintf(url, sizeof url, "%s/configure.toml", cfg->blueprint_base_url);
        if(bp_fetch_verify(url, cfg->blueprint_pubkey, "/tmp/oobe-configure.toml", err, sizeof err) == 0)
            bp_path = "/tmp/oobe-configure.toml";
    }
    if(bp_path) { char err[256]; W.bp = bp_load(bp_path, err, sizeof err); }
    W.ans = bp_answers_load("");
    W.n_stages = W.bp ? (int)bp_phase_order(W.bp, BP_PHASE_OOBE, W.ord) : 0;

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
    W.stepind = mk_kicker(header, "1 / 1");
    W.title = mk_label(header, "Welcome", W.f_title, PK_CREAM);

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

    W.kb = lv_keyboard_create(W.root);
    lv_obj_add_flag(W.kb, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(W.kb, w, clampi(h * 2 / 5, 180, 520));
    lv_obj_align(W.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(W.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(W.kb, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(W.kb, kb_event_cb, LV_EVENT_CANCEL, NULL);

    W.page = 0;
    render_page();
}
