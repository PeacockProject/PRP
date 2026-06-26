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
#include "prp_ui.h"
#include "prp_wizard.h"
#include "prp_net_ui.h"

static volatile sig_atomic_t g_stop = 0;
static uint32_t g_screen_w = 1080;
static uint32_t g_screen_h = 1920;
static int g_scale_pct = 100;
static int g_touch_div = 1;   // divide raw touch coords by the DPI factor (logical render)
static char g_logo_path[256] = {0};

// The evdev driver reports raw panel-pixel coords; when we render at a logical
// resolution (DPI upscale), scale touch down by the same factor so taps land on
// the right widget.
// evdev_read clamps its reported point to the (logical) display resolution, which
// destroys the real position when we render scaled. The driver keeps the raw,
// pre-clamp panel coordinate in these globals — use them and scale by the factor.
extern int evdev_root_x;
extern int evdev_root_y;
static void prp_evdev_read_scaled(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    evdev_read(drv, data);   // pumps events: updates evdev_root_x/y + button state
    if(g_touch_div > 1) {
        data->point.x = (lv_coord_t)(evdev_root_x / g_touch_div);
        data->point.y = (lv_coord_t)(evdev_root_y / g_touch_div);
        if(data->point.x >= (lv_coord_t)g_screen_w) data->point.x = (lv_coord_t)g_screen_w - 1;
        if(data->point.y >= (lv_coord_t)g_screen_h) data->point.y = (lv_coord_t)g_screen_h - 1;
    }
}

static prp_wizard_cfg_t g_wiz;
static void launch_wizard(void) { prp_wizard_show(&g_wiz); }
static void launch_network(void) {
    prp_net_ui_show((int)g_screen_w, (int)g_screen_h, g_scale_pct, NULL);
}



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
    "/usr/bin/prp-phoenix-clear 2>/dev/null; echo rebooting...; /sbin/busybox sync; /sbin/busybox reboot -f || /sbin/busybox reboot || echo reboot_failed";
static const char *const k_cmd_poweroff =
    "echo powering off...; /sbin/busybox sync; /sbin/busybox poweroff -f || /sbin/busybox poweroff || echo poweroff_failed";
static const char *const k_cmd_mount_subparts = "/usr/bin/prp-mount-peacock-subparts";

typedef struct {
    int fd;
    pid_t pid;
    bool running;
} prp_job_t;

static prp_job_t g_job = {.fd = -1, .pid = -1, .running = false};

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

static void start_cmd(const char *cmd) {
    if(!cmd) return;
    if(g_job.running) {
        prp_ui_log_appendf("\n[busy] previous command still running (pid=%d)\n", (int)g_job.pid);
        return;
    }

    prp_ui_log_appendf("\n$ %s\n", cmd);

    int pfd[2];
    if(pipe(pfd) != 0) {
        prp_ui_log_appendf("[err] pipe failed: %s\n", strerror(errno));
        return;
    }
    (void)fcntl(pfd[0], F_SETFL, O_NONBLOCK);

    pid_t pid = fork();
    if(pid < 0) {
        prp_ui_log_appendf("[err] fork failed: %s\n", strerror(errno));
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

#include "prp_theme.h"


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
            prp_ui_power_menu_show();
            g_pwr_menu_shown = true;
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

/* Build the "INSTALL TO" dropdown options ("a\nb\nc") by running prp-targets,
 * which lists installable partitions (by-name preferred, sdcard included). The
 * returned static buffer's first token per line is what prp-install resolves. */
static const char *enumerate_install_targets(void) {
    static char buf[2048];
    size_t off = 0;
    buf[0] = '\0';
    FILE *fp = popen("prp-targets 2>/dev/null", "r");
    if(fp) {
        char line[256];
        while(fgets(line, sizeof(line), fp)) {
            size_t n = strlen(line);
            while(n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
            if(n == 0) continue;
            if(off && off + 1 < sizeof(buf)) buf[off++] = '\n';
            for(size_t i = 0; i < n && off + 1 < sizeof(buf); i++) buf[off++] = line[i];
            buf[off] = '\0';
        }
        pclose(fp);
    }
    if(buf[0] == '\0')
        snprintf(buf, sizeof(buf), "%s", "(no install targets found)");
    return buf;
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

    // DPI scaling: render the UI at a LOGICAL resolution (native / factor) and
    // upscale factor× in the fbdev flush. Two wins at once — fewer pixels are
    // software-rendered (smoother swipes) and the whole UI is uniformly larger
    // (readable on a high-DPI panel like daisy's 1080x2280). The factor comes
    // from scale_pct (200 -> 2x). On the logical canvas the UI then lays out
    // neutrally (scale_pct reset to 100); the flush does the upscale + touch
    // auto-maps because the evdev driver scales to the display resolution.
    int dpi_factor = g_scale_pct / 100;
    if(dpi_factor < 1) dpi_factor = 1;
    if(dpi_factor > 1) {
        g_screen_w = (int)fb.width / dpi_factor;
        g_screen_h = (int)fb.height / dpi_factor;
        prp_fbdev_set_scale(dpi_factor);
        g_touch_div = dpi_factor;
        g_scale_pct = 100;
        fprintf(stderr, "prp-gui: DPI scale %dx -> logical %dx%d (panel %ux%u)\n",
                dpi_factor, g_screen_w, g_screen_h, fb.width, fb.height);
    }

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
    lv_disp_set_bg_color(disp, lv_color_hex(PK_BG));
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
    indev_drv.read_cb = prp_evdev_read_scaled;
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

    const char *logo_paths[12];
    size_t li = 0;
    if(g_logo_path[0]) logo_paths[li++] = g_logo_path;
    logo_paths[li++] = "/etc/prp/header_logo.png";
    logo_paths[li++] = "/etc/prp/logo_header.png";
    logo_paths[li++] = "/etc/header_logo.png";
    logo_paths[li++] = "/mnt/prp_rootfs/etc/prp/header_logo.png";
    logo_paths[li++] = "/mnt/prp_rootfs/etc/prp/logo_header.png";
    logo_paths[li++] = "/mnt/prp_rootfs/etc/header_logo.png";
    logo_paths[li++] = "header_logo.png";
    logo_paths[li++] = "logo_header.png";
    logo_paths[li] = NULL;

    const prp_ui_item_t buttons[] = {
        {"Terminal", "Open a command shell on the device", LV_SYMBOL_KEYBOARD,
         "setsid cttyhack /bin/sh </dev/tty1 >/dev/tty1 2>&1 &"},
        {"Remote access", "Connect from a computer over SSH (port 22)", LV_SYMBOL_WIFI,
         "PRP_SSH_ALLOW_BLANK_PASSWORD=1 PRP_SSH_PORT=22 /usr/bin/prp-svc-ssh >/tmp/prp-ssh.log 2>&1; /sbin/busybox sleep 1; if /sbin/busybox pidof dropbear >/dev/null 2>&1; then echo ssh_up port=22; else echo ssh_down; /sbin/busybox head -n 20 /tmp/prp-ssh.log; fi"},
        {"Mount system", "Mount the Peacock partitions", LV_SYMBOL_DRIVE, k_cmd_mount_subparts},
    };
    const prp_ui_item_t power_items[] = {
        {"Reboot", NULL, NULL, k_cmd_reboot},
        {"Power off", NULL, NULL, k_cmd_poweroff},
        {"Cancel", NULL, NULL, NULL},
    };

    prp_ui_cfg_t ui_cfg = {0};
    ui_cfg.screen_w = (int)g_screen_w;
    ui_cfg.screen_h = (int)g_screen_h;
    ui_cfg.scale_pct = g_scale_pct;
    ui_cfg.logo_paths = logo_paths;
    ui_cfg.buttons = buttons;
    ui_cfg.n_buttons = (int)(sizeof(buttons) / sizeof(buttons[0]));
    ui_cfg.power_items = power_items;
    ui_cfg.n_power = (int)(sizeof(power_items) / sizeof(power_items[0]));
    ui_cfg.run_cmd = start_cmd;

    static char reason_buf[512];
    if(read_file_trim("/etc/prp/reason", reason_buf, sizeof(reason_buf)) ||
       read_file_trim("/tmp/prp-reason", reason_buf, sizeof(reason_buf))) {
        ui_cfg.reason = reason_buf;   // set by the OS handoff when it diverts to recovery
    }
    ui_cfg.help_url = "https://wiki.peacockos.org/recovery";

    g_wiz.screen_w = (int)g_screen_w;
    g_wiz.screen_h = (int)g_screen_h;
    g_wiz.scale_pct = g_scale_pct;
    g_wiz.device_name = "this device";
    g_wiz.device_codename = "";
    g_wiz.flavors = "Arch\nDebian\nAlpine";
    g_wiz.inits = "systemd\nOpenRC";
    g_wiz.disks = enumerate_install_targets();   /* real partitions via prp-targets */
    g_wiz.wifi_ssids = "(Wi-Fi scan coming soon)";   /* prp-net wires real scanning */
    /* The wizard fetches the flavor list from genmirror's blueprint index (verified). */
    g_wiz.blueprint_pubkey = "/etc/feather/genmirror.pub";
    {
        /* genmirror blueprints base URL, written by assemble at /etc/peacock/blueprints-base
         * (never hardcoded here). Absent (e.g. odd builds) → wizard uses the bundle. */
        static char bp_base[256];
        FILE *bf = fopen("/etc/peacock/blueprints-base", "r");
        if(bf) {
            if(fgets(bp_base, sizeof bp_base, bf)) {
                bp_base[strcspn(bp_base, "\r\n")] = '\0';
                if(bp_base[0]) g_wiz.blueprint_base_url = bp_base;
            }
            fclose(bf);
        }
    }
    ui_cfg.on_install = launch_wizard;
    ui_cfg.on_network = launch_network;
    prp_ui_build(&ui_cfg);
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
                    prp_ui_log_append(buf, (size_t)n);
                    continue;
                }
                if(n == 0) {
                    int st = 0;
                    pid_t wr = waitpid(g_job.pid, &st, WNOHANG);
                    close(g_job.fd);
                    g_job.fd = -1;
                    g_job.running = false;
                    if(wr > 0) {
                        if(WIFEXITED(st)) prp_ui_log_appendf("[exit] code=%d\n", WEXITSTATUS(st));
                        else if(WIFSIGNALED(st)) prp_ui_log_appendf("[exit] signal=%d\n", WTERMSIG(st));
                        else prp_ui_log_appendf("[exit] done\n");
                    } else {
                        prp_ui_log_appendf("[exit] done\n");
                    }
                    break;
                }
                if(errno == EINTR) continue;
                if(errno == EAGAIN || errno == EWOULDBLOCK) break;
                prp_ui_log_appendf("[err] read: %s\n", strerror(errno));
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
