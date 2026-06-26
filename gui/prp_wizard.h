#ifndef PRP_WIZARD_H
#define PRP_WIZARD_H

// PRP install wizard: a full-screen, multi-step flow that asks the same things the
// desktop Peacock builder asks, then (Phase 5) drives the on-device install. In Phase 1
// the install step is MOCKED (a timer-driven fake progress) so the whole flow is
// exercisable in the SDL sim and on-device before the real backend exists.
//
// Options are passed as newline-separated strings (lv_dropdown format). Networking is
// mocked from `wifi_ssids` for now; real wpa_supplicant wiring comes with prp-net.

typedef struct {
    int screen_w;
    int screen_h;
    int scale_pct;
    const char *device_name;      // auto-detected, e.g. "Xiaomi Mi A2 Lite"
    const char *device_codename;  // e.g. "daisy"
    const char *flavors;          // fallback flavor list if the index fetch fails ("Arch\n...")
    const char *inits;            // "systemd\nOpenRC"
    const char *disks;            // target disks, e.g. "Internal storage (mmcblk0)"
    const char *wifi_ssids;       // mock scan results (newline-separated) until prp-net lands
    // Desktop/login-manager/account/timezone are NOT asked here — they're first-boot OOBE.
    const char *blueprint_base_url; // genmirror blueprints base, e.g. ".../blueprints/stable".
                                  // The wizard fetches <base>/index.toml (verified) for the flavor
                                  // list, so adding a flavor needs no PRP rebuild.
    const char *blueprint_pubkey; // trust anchor for the fetched index (genmirror.pub).
} prp_wizard_cfg_t;

// Launch the wizard as a full-screen overlay on the active screen.
void prp_wizard_show(const prp_wizard_cfg_t *cfg);

#endif // PRP_WIZARD_H
