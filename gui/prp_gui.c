// PRP framebuffer GUI using LVGL (fbdev) and Linux input events (evdev).
// Intended to be stored on PRP_ROOTFS overlay and auto-started by initramfs.

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LV_CONF_INCLUDE_SIMPLE 1
#include "lvgl/lvgl.h"

#include "lv_drivers/indev/evdev.h"

#include "prp_fbdev.h"
#include "prp_logo.h"

static volatile sig_atomic_t g_stop = 0;
static uint32_t g_screen_w = 1080;
static uint32_t g_screen_h = 1920;
static int g_scale_pct = 100;
static char g_logo_path[256] = {0};
static lv_img_dsc_t *g_logo_dsc = NULL;

static lv_obj_t *g_log_cont = NULL;
static lv_obj_t *g_log_label = NULL;
static char *g_log_buf = NULL;
static size_t g_log_len = 0;
static size_t g_log_cap = 0;

static lv_obj_t *g_pwr_overlay = NULL;

static int g_pwr_fd = -1;
static char g_pwr_input[128] = {0};
static char g_pwr_hint[256] = {0};
static int g_pwr_code = KEY_POWER;
static bool g_pwr_down = false;
static bool g_pwr_menu_shown = false;
static uint64_t g_pwr_down_ms = 0;
static bool g_touch_attached = false;
static uint64_t g_touch_retry_due_ms = 0;

static const char *const k_cmd_reboot =
    "echo rebooting...; /sbin/busybox sync; /sbin/busybox reboot -f || /sbin/busybox reboot || echo reboot_failed";
static const char *const k_cmd_poweroff =
    "echo powering off...; /sbin/busybox sync; /sbin/busybox poweroff -f || /sbin/busybox poweroff || echo poweroff_failed";
static const char *const k_cmd_mount_subparts = "/usr/bin/prp-mount-peacock-subparts";

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
    // xorshift64*
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

static uint64_t now_ms(void) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
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

    // Keep a bounded log buffer (retain the tail).
    const size_t max_keep = 32768;
    if(g_log_len > max_keep) {
        size_t keep = max_keep / 2;
        memmove(g_log_buf, g_log_buf + (g_log_len - keep), keep);
        g_log_len = keep;
        g_log_buf[g_log_len] = '\0';
    }

    // Ensure capacity.
    size_t need = g_log_len + n + 1;
    if(need > g_log_cap) {
        size_t new_cap = g_log_cap;
        while(new_cap < need) new_cap *= 2;
        if(new_cap > 131072) new_cap = 131072;
        if(new_cap < need) {
            // If still too small, drop the oldest half and retry.
            if(g_log_len > 0) {
                size_t keep = g_log_len / 2;
                memmove(g_log_buf, g_log_buf + (g_log_len - keep), keep);
                g_log_len = keep;
                g_log_buf[g_log_len] = '\0';
            }
            need = g_log_len + n + 1;
        }
        if(need > g_log_cap) {
            char *nb = (char *)realloc(g_log_buf, new_cap);
            if(nb) {
                g_log_buf = nb;
                g_log_cap = new_cap;
            }
        } else {
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

static void start_cmd(const char *cmd) {
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
        setenv("PATH", "/sbin:/bin:/usr/sbin:/usr/bin", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        dprintf(STDERR_FILENO, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    close(pfd[1]);
    g_job.fd = pfd[0];
    g_job.pid = pid;
    g_job.running = (pid > 0);
}

static bool read_file_trim(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if(n <= 0) return false;
    buf[n] = '\0';
    while(n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ' || buf[n - 1] == '\t')) {
        buf[n - 1] = '\0';
        n--;
    }
    return true;
}

static int strcasestr_like(const char *hay, const char *needle) {
    if(!hay || !needle || !*needle) return 0;
    size_t nl = strlen(needle);
    for(const char *p = hay; *p; p++) {
        if(strncasecmp(p, needle, nl) == 0) return 1;
    }
    return 0;
}

static int bit_is_set(const unsigned long *bits, int bit) {
    return (bits[bit / (int)(8 * sizeof(unsigned long))] >> (bit % (int)(8 * sizeof(unsigned long)))) & 1UL;
}

static int score_touch_name(const char *name) {
    int score = 0;
    if(!name || !*name) return score;

    if(strcasestr_like(name, "touch") || strcasestr_like(name, "goodix") || strcasestr_like(name, "synaptics") ||
       strcasestr_like(name, "atmel") || strcasestr_like(name, "mxt") || strcasestr_like(name, "fts") ||
       strcasestr_like(name, "ft5") || strcasestr_like(name, "ft6")) {
        score += 6;
    }
    if(strcasestr_like(name, "gpio-keys") || strcasestr_like(name, "key") || strcasestr_like(name, "power")) {
        score -= 8;
    }
    return score;
}

static bool pick_touch_event(char *out_path, size_t out_sz) {
    char best_path[64] = {0};
    int best_score = -9999;

    for(int i = 0; i < 32; i++) {
        char dev_path[64];
        struct stat st;
        snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
        if(stat(dev_path, &st) != 0) continue;

        int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
        if(fd < 0) continue;

        unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
        unsigned long abs_bits[(ABS_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
        unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
#ifdef INPUT_PROP_MAX
        unsigned long prop_bits[(INPUT_PROP_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
#endif
        memset(ev_bits, 0, sizeof(ev_bits));
        memset(abs_bits, 0, sizeof(abs_bits));
        memset(key_bits, 0, sizeof(key_bits));
#ifdef INPUT_PROP_MAX
        memset(prop_bits, 0, sizeof(prop_bits));
#endif

        if(ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !bit_is_set(ev_bits, EV_ABS)) {
            close(fd);
            continue;
        }
        if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            close(fd);
            continue;
        }

        bool has_abs_xy = bit_is_set(abs_bits, ABS_X) && bit_is_set(abs_bits, ABS_Y);
        bool has_mt_xy = bit_is_set(abs_bits, ABS_MT_POSITION_X) && bit_is_set(abs_bits, ABS_MT_POSITION_Y);
        if(!has_abs_xy && !has_mt_xy) {
            close(fd);
            continue;
        }

        bool has_btn_touch = false;
        if(bit_is_set(ev_bits, EV_KEY) && ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) >= 0) {
            has_btn_touch = bit_is_set(key_bits, BTN_TOUCH);
        }

        bool direct = false;
#ifdef INPUT_PROP_DIRECT
        if(ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) >= 0) {
            direct = bit_is_set(prop_bits, INPUT_PROP_DIRECT);
        }
#endif

        char name[256] = {0};
        if(ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            name[0] = '\0';
        }
        close(fd);

        int name_score = score_touch_name(name);
        if(!direct && !has_btn_touch && !has_mt_xy && name_score <= 0) {
            continue;
        }

        int score = 0;
        if(direct) score += 8;
        if(has_btn_touch) score += 4;
        if(has_mt_xy) score += 3;
        if(has_abs_xy) score += 2;
        score += name_score;

        fprintf(stderr,
                "prp-gui: touch cand %s name='%s' abs_xy=%d mt_xy=%d btn_touch=%d direct=%d score=%d\n",
                dev_path, name[0] ? name : "unknown", has_abs_xy ? 1 : 0, has_mt_xy ? 1 : 0, has_btn_touch ? 1 : 0,
                direct ? 1 : 0, score);

        if(score > best_score) {
            best_score = score;
            snprintf(best_path, sizeof(best_path), "%s", dev_path);
        }
    }

    if(best_path[0] && best_score > -9999) {
        snprintf(out_path, out_sz, "%s", best_path);
        fprintf(stderr, "prp-gui: touch selected %s (score=%d)\n", out_path, best_score);
        return true;
    }

    return false;
}

static void try_late_touch_attach(void) {
    if(g_touch_attached) return;
    uint64_t now = now_ms();
    if(now < g_touch_retry_due_ms) return;
    g_touch_retry_due_ms = now + 1000;

    char ev_path[128] = {0};
    if(!pick_touch_event(ev_path, sizeof(ev_path))) return;
    if(!evdev_set_file(ev_path)) return;

    g_touch_attached = true;
    fprintf(stderr, "prp-gui: touch input attached late: %s\n", ev_path);
}

static void btn_cmd_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    start_cmd(cmd);
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
    // Large panels (e.g. 1080x1920) need a bump even at 100%.
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

static void power_menu_hide(void);

static void power_menu_cmd_cb(lv_event_t *e) {
    const char *cmd = (const char *)lv_event_get_user_data(e);
    power_menu_hide();
    if(cmd && *cmd) start_cmd(cmd);
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

static void power_menu_show(void) {
    if(g_pwr_overlay) return;

    lv_obj_t *scr = lv_scr_act();
    const int w = (int)g_screen_w;
    const int h = (int)g_screen_h;
    const int scale = clampi(g_scale_pct, 50, 200);
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

    struct {
        const char *label;
        const char *cmd;
    } acts[] = {
        {"Reboot", k_cmd_reboot},
        {"Power off", k_cmd_poweroff},
        {"Cancel", NULL},
    };

    for(size_t i = 0; i < sizeof(acts) / sizeof(acts[0]); i++) {
        lv_obj_t *btn = lv_btn_create(dlg);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, clampi(h / 14, 72, 160));
        lv_obj_set_style_bg_color(btn, lv_color_make(0x10, 0x10, 0x10), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_add_event_cb(btn, power_menu_cmd_cb, LV_EVENT_CLICKED, (void *)acts[i].cmd);

        lv_obj_t *lab = lv_label_create(btn);
        lv_label_set_text(lab, acts[i].label);
        lv_obj_set_style_text_font(lab, font_body, 0);
        lv_obj_set_style_text_color(lab, lv_color_white(), 0);
        lv_obj_center(lab);
    }
}

static void appbar_long_press_cb(lv_event_t *e) {
    (void)e;
    power_menu_show();
}

static int power_hint_score(const char *name, const char *hints) {
    if(!name || !*name || !hints || !*hints) return 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", hints);
    int score = 0;
    for(char *tok = strtok(buf, ",;| "); tok; tok = strtok(NULL, ",;| ")) {
        if(*tok && strcasestr_like(name, tok)) score += 8;
    }
    return score;
}

static bool pick_power_event(char *out_path, size_t out_sz, const char *exclude_path) {
    char best_path[64] = {0};
    int best_score = -9999;

    for(int i = 0; i < 32; i++) {
        char dev_path[64];
        struct stat st;
        snprintf(dev_path, sizeof(dev_path), "/dev/input/event%d", i);
        if(stat(dev_path, &st) != 0) continue;
        if(exclude_path && *exclude_path && strcmp(dev_path, exclude_path) == 0) continue;

        int fd = open(dev_path, O_RDONLY | O_NONBLOCK);
        if(fd < 0) continue;

        unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
        unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
        memset(ev_bits, 0, sizeof(ev_bits));
        memset(key_bits, 0, sizeof(key_bits));

        if(ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0 || !bit_is_set(ev_bits, EV_KEY) ||
           ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            close(fd);
            continue;
        }

        char name[256] = {0};
        if(ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            name[0] = '\0';
        }

        bool has_power = bit_is_set(key_bits, KEY_POWER);
        bool has_wakeup = bit_is_set(key_bits, KEY_WAKEUP);
        bool has_sleep = bit_is_set(key_bits, KEY_SLEEP);
        close(fd);

        int score = 0;
        int hint_score = power_hint_score(name, g_pwr_hint);
        if(has_power) score += 10;
        if(has_wakeup) score += 2;
        if(has_sleep) score += 1;
        if(strcasestr_like(name, "power") || strcasestr_like(name, "pwr") ||
           strcasestr_like(name, "gpio-keys")) {
            score += 4;
        }
        score += hint_score;

        fprintf(stderr, "prp-gui: power cand %s name='%s' hint=%d score=%d\n", dev_path, name[0] ? name : "unknown",
                hint_score, score);
        if(score > best_score) {
            best_score = score;
            snprintf(best_path, sizeof(best_path), "%s", dev_path);
        }
    }
    if(best_path[0] && best_score > 0) {
        snprintf(out_path, out_sz, "%s", best_path);
        fprintf(stderr, "prp-gui: power selected %s (score=%d)\n", out_path, best_score);
        return true;
    }
    return false;
}

static void power_key_poll(void) {
    if(g_pwr_fd < 0) return;

    struct input_event ev;
    for(;;) {
        ssize_t n = read(g_pwr_fd, &ev, sizeof(ev));
        if(n == (ssize_t)sizeof(ev)) {
            if(ev.type == EV_KEY && ev.code == g_pwr_code) {
                if(ev.value == 1) {
                    g_pwr_down = true;
                    g_pwr_menu_shown = false;
                    g_pwr_down_ms = now_ms();
                } else if(ev.value == 0) {
                    g_pwr_down = false;
                    g_pwr_menu_shown = false;
                }
            } else if(ev.type == EV_KEY && ev.value == 1) {
                fprintf(stderr, "prp-gui: power input key press code=%u (expect=%d)\n", ev.code, g_pwr_code);
            }
            continue;
        }
        if(n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if(n < 0 && errno == EINTR) continue;
        break;
    }

    if(g_pwr_down && !g_pwr_menu_shown) {
        if(now_ms() - g_pwr_down_ms >= 800) {
            power_menu_show();
            g_pwr_menu_shown = true;
        }
    }
}

static void build_ui(void) {
    const int w = (int)g_screen_w;
    const int h = (int)g_screen_h;
    const int scale = clampi(g_scale_pct, 50, 200);
    const int margin = clampi((h / 36) * scale / 100, 8, 80);
    const int gap = clampi((h / 64) * scale / 100, 6, 48);
    const int appbar_pad = clampi(gap / 2, 8, 28);

    const lv_font_t *font_title = pick_font_title(scale, w, h);
    const lv_font_t *font_hint = pick_font_hint(scale, w, h);
    const lv_font_t *font_body = pick_font_body(scale, w, h);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    /* Important: ensure the screen is opaque. On real fbdev, leaving bg_opa at default
       can make the background effectively "transparent", showing whatever the kernel/boot
       stage left in the framebuffer (often red). */
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

    // Optional logo image loaded from the prepared PRP runtime.
    if(!g_logo_dsc) {
        const char *paths[12];
        size_t pi = 0;
        if(g_logo_path[0]) paths[pi++] = g_logo_path;
        paths[pi++] = "/etc/prp/header_logo.png";
        paths[pi++] = "/etc/prp/logo_header.png";
        paths[pi++] = "/etc/header_logo.png";
        paths[pi++] = "/mnt/prp_rootfs/etc/prp/header_logo.png";
        paths[pi++] = "/mnt/prp_rootfs/etc/prp/logo_header.png";
        paths[pi++] = "/mnt/prp_rootfs/etc/header_logo.png";
        paths[pi++] = "header_logo.png";
        paths[pi++] = "logo_header.png";
        paths[pi] = NULL;
        g_logo_dsc = prp_logo_try_load(paths);
    }

    lv_obj_t *title = lv_label_create(appbar);
    lv_label_set_text(title, "PRP Recovery");
    lv_obj_set_style_text_font(title, font_title, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
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

    // Hidden "motto" below the AppBar contents. Visible only if the user scrolls the header.
    lv_obj_t *motto = lv_label_create(appbar);
    lv_label_set_text(motto, pick_motto());
    lv_label_set_long_mode(motto, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(motto, w - 2 * appbar_pad);
    lv_obj_set_style_text_font(motto, font_hint, 0);
    lv_obj_set_style_text_color(motto, lv_color_make(0xE0, 0xE0, 0xE0), 0);
    lv_obj_align(motto, LV_ALIGN_TOP_MID, 0, appbar_h + appbar_pad);

    struct {
        const char *label;
        const char *cmd;
    } buttons[] = {
        {"Shell (tty1)", "setsid cttyhack /bin/sh </dev/tty1 >/dev/tty1 2>&1 &"},
        {"Start SSH", "PRP_SSH_ALLOW_BLANK_PASSWORD=1 PRP_SSH_PORT=22 /usr/bin/prp-svc-ssh >/tmp/prp-ssh.log 2>&1; /sbin/busybox sleep 1; if /sbin/busybox pidof dropbear >/dev/null 2>&1; then echo ssh_up port=22; else echo ssh_down; /sbin/busybox head -n 20 /tmp/prp-ssh.log; fi"},
        {"Mount Peacock", k_cmd_mount_subparts},
    };
    const int btn_cols = 2;
    const int btn_count = (int)(sizeof(buttons) / sizeof(buttons[0]));
    const int btn_rows = (btn_count + btn_cols - 1) / btn_cols;

    // Layout: buttons in the upper area, "stdout" log fixed at the bottom.
    const int top_y = appbar_h + sep_h + margin;

    // Bottom "stdout" area (max half screen height).
    const int log_h_default = clampi((h / 3) * scale / 100, 120, h / 2);

    // Prefer a large, tappable button height on phones.
    int btn_h_want = clampi((h / 8) * scale / 100, 96, 320);
    const int btn_area_h_want = btn_h_want * btn_rows + gap * (btn_rows - 1);

    // Compute how much log height we can afford if we keep buttons large.
    int log_h_max_for_btn = (h - margin) - top_y - gap - btn_area_h_want;
    if(log_h_max_for_btn > h / 2) log_h_max_for_btn = h / 2;

    int log_h = log_h_default;
    if(log_h_max_for_btn >= 120) {
        // Keep buttons big; cap the log height accordingly.
        if(log_h > log_h_max_for_btn) log_h = log_h_max_for_btn;
    } else {
        // Not enough space: keep a minimum log height and shrink buttons to fit.
        log_h = 120;
        int available = (h - margin - log_h) - top_y - gap;
        int btn_h_max = (available - gap * (btn_rows - 1)) / btn_rows;
        btn_h_want = clampi(btn_h_want, 64, btn_h_max > 0 ? btn_h_max : 64);
    }

    const int btn_area_h = btn_h_want * btn_rows + gap * (btn_rows - 1);

    // Stdout window (bottom). Shows output of actions.
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

    const int btn_h = btn_h_want;
    const int btn_w = (inner_w - gap) / 2;

    for(int row = 0; row < btn_rows; row++) {
        lv_obj_t *rowc = lv_obj_create(btn_area);
        lv_obj_set_style_bg_color(rowc, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(rowc, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(rowc, 0, 0);
        lv_obj_set_style_pad_all(rowc, 0, 0);
        lv_obj_set_size(rowc, lv_pct(100), btn_h);
        lv_obj_set_flex_flow(rowc, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(rowc, gap, 0);

        for(int col = 0; col < btn_cols; col++) {
            int idx = row * btn_cols + col;
            if(idx >= btn_count) continue;
            lv_obj_t *btn = lv_btn_create(rowc);
            lv_obj_set_size(btn, btn_w, btn_h);
            lv_obj_set_style_bg_color(btn, lv_color_make(0x10, 0x10, 0x10), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(btn, lv_color_white(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_set_style_radius(btn, 8, 0);
            lv_obj_add_event_cb(btn, btn_cmd_cb, LV_EVENT_CLICKED, (void *)buttons[idx].cmd);

            lv_obj_t *lab = lv_label_create(btn);
            lv_label_set_text(lab, buttons[idx].label);
            lv_obj_set_style_text_font(lab, font_body, 0);
            lv_obj_center(lab);
        }
    }
}

typedef struct {
    char fbdev[128];
    char input[128];
    char power_input[128];
    char power_hint[256];
    int power_code;
    char config_path[256];
    char logo[256];
    int scale_pct;
} prp_gui_cfg_t;

static void cfg_init(prp_gui_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->fbdev, sizeof(cfg->fbdev), "%s", "/dev/fb0");
    cfg->scale_pct = 100;
    cfg->power_code = KEY_POWER;
}

static void strtrim_inplace(char *s) {
    if(!s) return;
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        n--;
    }
    char *p = s;
    while(*p == ' ' || *p == '\t') p++;
    if(p != s) memmove(s, p, strlen(p) + 1);
}

static void cfg_apply_kv(prp_gui_cfg_t *cfg, const char *k, const char *v) {
    if(!cfg || !k || !v) return;
    if(strcasecmp(k, "FBDEV") == 0) {
        snprintf(cfg->fbdev, sizeof(cfg->fbdev), "%s", v);
        return;
    }
    if(strcasecmp(k, "INPUT") == 0 || strcasecmp(k, "EVDEV") == 0) {
        snprintf(cfg->input, sizeof(cfg->input), "%s", v);
        return;
    }
    if(strcasecmp(k, "POWER_INPUT") == 0 || strcasecmp(k, "POWERKEY") == 0) {
        snprintf(cfg->power_input, sizeof(cfg->power_input), "%s", v);
        return;
    }
    if(strcasecmp(k, "POWER_HINT") == 0 || strcasecmp(k, "POWER_NAME_HINT") == 0 ||
       strcasecmp(k, "POWER_HINTS") == 0) {
        snprintf(cfg->power_hint, sizeof(cfg->power_hint), "%s", v);
        return;
    }
    if(strcasecmp(k, "POWER_CODE") == 0 || strcasecmp(k, "POWER_KEY_CODE") == 0) {
        cfg->power_code = atoi(v);
        return;
    }
    if(strcasecmp(k, "SCALE") == 0 || strcasecmp(k, "SCALE_PCT") == 0) {
        cfg->scale_pct = atoi(v);
        return;
    }
    if(strcasecmp(k, "LOGO") == 0 || strcasecmp(k, "LOGO_PATH") == 0) {
        snprintf(cfg->logo, sizeof(cfg->logo), "%s", v);
        return;
    }
}

static void cfg_load_file(prp_gui_cfg_t *cfg, const char *path) {
    if(!cfg || !path || !*path) return;
    FILE *f = fopen(path, "r");
    if(!f) return;
    char line[512];
    while(fgets(line, sizeof(line), f)) {
        strtrim_inplace(line);
        if(line[0] == '\0' || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if(!eq) continue;
        *eq = '\0';
        char *k = line;
        char *v = eq + 1;
        strtrim_inplace(k);
        strtrim_inplace(v);
        if(k[0] == '\0' || v[0] == '\0') continue;
        cfg_apply_kv(cfg, k, v);
    }
    fclose(f);
}

static void cfg_apply_env(prp_gui_cfg_t *cfg) {
    const char *v = getenv("PRP_GUI_FBDEV");
    if(v && *v) snprintf(cfg->fbdev, sizeof(cfg->fbdev), "%s", v);
    v = getenv("PRP_GUI_INPUT");
    if(v && *v) snprintf(cfg->input, sizeof(cfg->input), "%s", v);
    v = getenv("PRP_GUI_POWER_INPUT");
    if(v && *v) snprintf(cfg->power_input, sizeof(cfg->power_input), "%s", v);
    v = getenv("PRP_GUI_POWER_HINT");
    if(v && *v) snprintf(cfg->power_hint, sizeof(cfg->power_hint), "%s", v);
    v = getenv("PRP_GUI_POWER_CODE");
    if(v && *v) cfg->power_code = atoi(v);
    v = getenv("PRP_GUI_SCALE");
    if(v && *v) cfg->scale_pct = atoi(v);
    v = getenv("PRP_GUI_LOGO");
    if(v && *v) snprintf(cfg->logo, sizeof(cfg->logo), "%s", v);
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--config PATH] [--fbdev PATH] [--input PATH] [--scale PCT]\\n", argv0);
}

int main(int argc, char **argv) {
    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    prp_gui_cfg_t cfg;
    cfg_init(&cfg);
    cfg_apply_env(&cfg);

    // Default search path prefers the active runtime, then falls back to compatibility paths.
    snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", "/etc/prp-gui.conf");
    cfg_load_file(&cfg, cfg.config_path);
    cfg_load_file(&cfg, "/etc/prp/prp-gui.conf");
    cfg_load_file(&cfg, "/etc/prp-gui.conf");
    cfg_load_file(&cfg, "/mnt/prp_rootfs/etc/prp-gui.conf");

    for(int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if(strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(cfg.config_path, sizeof(cfg.config_path), "%s", argv[++i]);
            cfg_load_file(&cfg, cfg.config_path);
        } else if(strcmp(argv[i], "--fbdev") == 0 && i + 1 < argc) {
            snprintf(cfg.fbdev, sizeof(cfg.fbdev), "%s", argv[++i]);
        } else if(strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            snprintf(cfg.input, sizeof(cfg.input), "%s", argv[++i]);
        } else if(strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            cfg.scale_pct = atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    g_scale_pct = cfg.scale_pct;
    if(cfg.logo[0]) snprintf(g_logo_path, sizeof(g_logo_path), "%s", cfg.logo);
    if(cfg.power_input[0]) snprintf(g_pwr_input, sizeof(g_pwr_input), "%s", cfg.power_input);
    if(cfg.power_hint[0]) snprintf(g_pwr_hint, sizeof(g_pwr_hint), "%s", cfg.power_hint);
    g_pwr_code = cfg.power_code;

    lv_init();
    prp_fbdev_t fb;
    if(!prp_fbdev_init(&fb, cfg.fbdev)) {
        fprintf(stderr, "prp-gui: fbdev init failed (%s)\n", cfg.fbdev);
        return 1;
    }
    // Ensure a deterministic starting point: some boot stages leave fb0 filled with red.
    prp_fbdev_clear(&fb, 0x0000);
    g_screen_w = fb.width;
    g_screen_h = fb.height;

    lv_disp_draw_buf_t draw_buf;
    // Modest draw buffer: fixed number of lines to keep memory bounded.
    const uint32_t buf_lines = 64;
    size_t buf_px = (size_t)g_screen_w * (size_t)buf_lines;
    lv_color_t *buf1 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
    lv_color_t *buf2 = (lv_color_t *)malloc(buf_px * sizeof(lv_color_t));
    if(!buf1 || !buf2) {
        fprintf(stderr, "prp-gui: out of memory\n");
        return 1;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, (uint32_t)buf_px);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = prp_fbdev_flush;
    disp_drv.hor_res = (lv_coord_t)g_screen_w;
    disp_drv.ver_res = (lv_coord_t)g_screen_h;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    // Be explicit about an opaque black display background to avoid inheriting whatever
    // a previous boot stage left in fb0.
    lv_disp_set_bg_color(disp, lv_color_black());
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);

    // Input (touch)
    evdev_init();
    char ev_path[128] = {0};
    bool ev_ok = false;
    if(cfg.input[0]) {
        snprintf(ev_path, sizeof(ev_path), "%s", cfg.input);
        ev_ok = evdev_set_file(ev_path);
        if(!ev_ok) {
            fprintf(stderr, "prp-gui: configured touch input failed: %s\n", ev_path);
            ev_path[0] = '\0';
        }
    }
    if(!ev_ok) {
        // Input nodes can appear slightly after fbdev/UI startup on some boots.
        for(int i = 0; i < 30 && !ev_ok; i++) {
            if(pick_touch_event(ev_path, sizeof(ev_path))) {
                ev_ok = evdev_set_file(ev_path);
            }
            if(!ev_ok) usleep(200000);
        }
    }
    if(ev_ok) {
        fprintf(stderr, "prp-gui: touch input active: %s\n", ev_path);
    } else {
        fprintf(stderr, "prp-gui: touch input unavailable\n");
    }
    g_touch_attached = ev_ok;
    g_touch_retry_due_ms = now_ms() + 1000;
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = evdev_read;
    lv_indev_drv_register(&indev_drv);

    // Optional power key input device for Android-style long-press power menu.
    char pwr_path[128] = {0};
    if(g_pwr_input[0]) {
        snprintf(pwr_path, sizeof(pwr_path), "%s", g_pwr_input);
    } else if(pick_power_event(pwr_path, sizeof(pwr_path), ev_path[0] ? ev_path : NULL)) {
        // auto-detected
    }
    if(pwr_path[0]) {
        g_pwr_fd = open(pwr_path, O_RDONLY | O_NONBLOCK);
        if(g_pwr_fd >= 0) {
            fprintf(stderr, "prp-gui: power input active: %s code=%d\n", pwr_path, g_pwr_code);
        }
    }

    build_ui();
    // Force an initial full-screen redraw even if LVGL decides the invalid area is smaller.
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(disp);

    while(!g_stop) {
        try_late_touch_attach();
        power_key_poll();
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
                    pid_t wr = waitpid(g_job.pid, &st, WNOHANG);
                    close(g_job.fd);
                    g_job.fd = -1;
                    g_job.running = false;
                    if(wr > 0) {
                        if(WIFEXITED(st)) log_appendf("[exit] code=%d\n", WEXITSTATUS(st));
                        else if(WIFSIGNALED(st)) log_appendf("[exit] signal=%d\n", WTERMSIG(st));
                        else log_appendf("[exit] done\n");
                    } else {
                        log_appendf("[exit] done\n");
                    }
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

    if(g_pwr_fd >= 0) close(g_pwr_fd);
    prp_fbdev_deinit(&fb);
    return 0;
}
