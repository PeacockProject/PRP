// PRP GUI host simulator: SDL2 display/input backend for the shared UI (prp_ui.c).
// Lets us iterate on the PRP GUI without flashing a phone. Layout lives in prp_ui.c.
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

#include "prp_ui.h"
#include "prp_wizard.h"
#include "prp_net_ui.h"

static volatile sig_atomic_t g_stop = 0;
static int g_scale_pct = 100;

static prp_wizard_cfg_t g_wiz;
static void launch_wizard(void) { prp_wizard_show(&g_wiz); }
static void launch_network(void) {
    prp_net_ui_show(SDL_HOR_RES, SDL_VER_RES, g_scale_pct, g_wiz.wifi_ssids);
}

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

// Backend command runner. Recognises two sim sentinels:
//   "@quit"        -> close the simulator
//   "@sim:<label>" -> just log it (no real action on the host)
// anything else is run through /bin/sh and its output streamed to the log console.
static void sim_run_cmd(const char *cmd) {
    if(!cmd) return;
    if(strcmp(cmd, "@quit") == 0) {
        g_stop = 1;
        return;
    }
    if(strncmp(cmd, "@sim:", 5) == 0) {
        prp_ui_log_appendf("\n[host] %s (simulated)\n", cmd + 5);
        return;
    }
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
        setenv("PATH", "/usr/bin:/bin:/usr/sbin:/sbin", 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }

    close(pfd[1]);
    g_job.fd = pfd[0];
    g_job.pid = pid;
    g_job.running = true;
}

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--scale PCT]\n", argv0);
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
    lv_disp_set_bg_opa(disp, LV_OPA_COVER);

    /* Input: mouse as touch */
    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    const char *envp = getenv("PRP_GUI_LOGO");
    const char *logo_paths[6];
    size_t li = 0;
    if(envp && *envp) logo_paths[li++] = envp;
    logo_paths[li++] = "header_logo.png";
    logo_paths[li++] = "logo_header.png";
    logo_paths[li++] = "../header_logo.png";
    logo_paths[li++] = "/etc/prp/header_logo.png";
    logo_paths[li] = NULL;

    static const prp_ui_item_t buttons[] = {
        {"Terminal", "Open a host shell", LV_SYMBOL_KEYBOARD,
         "setsid -f sh -lc 'xterm -e sh || alacritty -e sh || gnome-terminal -- sh || konsole -e sh'"},
        {"Config", "Print the GUI config", LV_SYMBOL_SETTINGS, "sh -lc 'echo PRP_GUI_SCALE=$PRP_GUI_SCALE'"},
        {"Quit", "Close the simulator", LV_SYMBOL_CLOSE, "@quit"},
        {"No-op", "Do nothing", LV_SYMBOL_OK, "true"},
    };
    static const prp_ui_item_t power_items[] = {
        {"Reboot", NULL, NULL, "@sim:Reboot"},
        {"Power off", NULL, NULL, "@sim:Power off"},
        {"Cancel", NULL, NULL, NULL},
    };

    prp_ui_cfg_t cfg = {0};
    cfg.screen_w = SDL_HOR_RES;
    cfg.screen_h = SDL_VER_RES;
    cfg.scale_pct = g_scale_pct;
    cfg.logo_paths = logo_paths;
    cfg.buttons = buttons;
    cfg.n_buttons = (int)(sizeof(buttons) / sizeof(buttons[0]));
    cfg.power_items = power_items;
    cfg.n_power = (int)(sizeof(power_items) / sizeof(power_items[0]));
    cfg.run_cmd = sim_run_cmd;
    cfg.reason = "Demo: the normal system failed to boot (kernel panic during early init).";
    cfg.help_url = "https://wiki.peacockos.org/recovery";

    g_wiz.screen_w = SDL_HOR_RES;
    g_wiz.screen_h = SDL_VER_RES;
    g_wiz.scale_pct = g_scale_pct;
    g_wiz.device_name = "QEMU x86_64";
    g_wiz.device_codename = "qemu-x86_64";
    g_wiz.flavors = "Arch\nDebian\nAlpine";
    g_wiz.inits = "systemd\nOpenRC";
    g_wiz.desktops = "None\nXFCE\nKDE Plasma\nGNOME\nMATE\nCinnamon\nLXQt";
    g_wiz.dms = "None\nSDDM\nLightDM\ngreetd\nGDM\nly";
    g_wiz.disks = "Internal storage (sda)";
    g_wiz.wifi_ssids = "PeacockNet\nHome Wi-Fi 5G\nguest\n(other…)";
    cfg.on_install = launch_wizard;
    cfg.on_network = launch_network;
    prp_ui_build(&cfg);

    while(!g_stop) {
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
                    (void)waitpid(g_job.pid, &st, WNOHANG);
                    close(g_job.fd);
                    g_job.fd = -1;
                    g_job.running = false;
                    if(WIFEXITED(st)) prp_ui_log_appendf("[exit] code=%d\n", WEXITSTATUS(st));
                    else if(WIFSIGNALED(st)) prp_ui_log_appendf("[exit] signal=%d\n", WTERMSIG(st));
                    else prp_ui_log_appendf("[exit] done\n");
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

    return 0;
}
