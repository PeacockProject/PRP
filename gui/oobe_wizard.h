// PeacockOS first-boot OOBE wizard (framebuffer, LVGL). Same chrome as the PRP install wizard, but
// the middle is one screen per oobe-phase stage of the flavor's configure.toml (polymorphic fields)
// and the Progress step applies the blueprint by forking `peacock-oobe --apply` and parsing its
// STEP/PROGRESS/LOG/DONE/ERROR protocol into the bar. With no peacock-oobe present (SDL sim) it
// falls back to a timer-driven mock so the flow stays exercisable on the host.
#ifndef PRP_OOBE_WIZARD_H
#define PRP_OOBE_WIZARD_H

typedef struct {
    int screen_w;
    int screen_h;
    int scale_pct;
    const char *device_name;     // shown on the welcome screen
    const char *flavor;          // active flavor (title + apply target)
    const char *root;            // /flavors/<flavor> — apply's --root

    // Blueprint source for the configure.toml + step scripts. On device: a verified genmirror base
    // URL + pubkey. In the sim: a local dir (blueprint_local) is used for rendering and the apply is
    // mocked. blueprint_local wins when set.
    const char *blueprint_base_url; // e.g. <genmirror>/blueprints/stable/arch
    const char *blueprint_pubkey;   // /etc/feather/genmirror.pub
    const char *blueprint_local;    // local dir holding configure.toml (sim / offline)
} oobe_cfg_t;

// Build + run the OOBE as a full-screen overlay on the active screen.
void prp_oobe_show(const oobe_cfg_t *cfg);

#endif /* PRP_OOBE_WIZARD_H */
