#ifndef PRP_UI_H
#define PRP_UI_H

// Shared PRP recovery UI: the whole on-screen layout + styling lives here, once.
// Backends (prp_gui.c = device fbdev, prp_gui_sdl.c = desktop SDL sim) only set up
// their display/input drivers and the main loop, then hand a prp_ui_cfg_t to
// prp_ui_build(). This is the single source of truth for how the GUI is structured.

#include <stddef.h>

typedef struct {
    const char *label;   // short, friendly title
    const char *desc;    // one-line explanation (optional, NULL to omit)
    const char *icon;    // an LV_SYMBOL_* glyph (optional, NULL to omit)
    const char *cmd;     // passed to cfg.run_cmd on tap; NULL = no command
} prp_ui_item_t;

typedef struct {
    int screen_w;
    int screen_h;
    int scale_pct;
    const char *const *logo_paths;     // NULL-terminated header-logo search list (or NULL)
    const prp_ui_item_t *buttons;      // action grid (2 columns)
    int n_buttons;
    const prp_ui_item_t *power_items;   // power-menu rows; an item with cmd==NULL renders
    int n_power;                        // as a quiet "Cancel" that just closes the menu
    void (*run_cmd)(const char *cmd);   // backend runs the command for a tapped row
    const char *reason;                 // why recovery was entered (NULL = generic message)
    const char *help_url;               // wiki recovery guide; shown as a QR code (NULL to omit)
    void (*on_install)(void);           // launch the install wizard; NULL hides the Install row
    void (*on_network)(void);           // open the Wi-Fi connect overlay; NULL hides the row
} prp_ui_cfg_t;

// Build the recovery screen (header + logo, buttons, log console, power-menu wiring).
void prp_ui_build(const prp_ui_cfg_t *cfg);

// Open the power menu (e.g. from a hardware power-key long-press in the device backend).
void prp_ui_power_menu_show(void);

// Append backend/command output into the on-screen log console.
void prp_ui_log_append(const char *s, size_t n);
void prp_ui_log_appendf(const char *fmt, ...);

#endif // PRP_UI_H
