#ifndef PRP_THEME_H
#define PRP_THEME_H

/* Shared peacock-maximal palette for the PRP GUI. Used by BOTH the on-device fbdev UI
   (prp_gui.c) and the desktop SDL simulator (prp_gui_sdl.c) so the two never drift.
   Colours match the site / wiki (see PeacockProject/site). LVGL bitmap fonts stay
   Montserrat for now; converting Instrument Serif / Hanken Grotesk to LVGL is separate. */

#define PK_BG      0x070B10   /* near-black page background        */
#define PK_PANEL   0x0E1620   /* raised surface (buttons, dialog)  */
#define PK_PANEL2  0x0B1219   /* sunken surface (console/log)      */
#define PK_CREAM   0xF4F1E8   /* primary text                      */
#define PK_DIM     0x9AA7B3   /* secondary text                    */
#define PK_LINE    0x1C2733   /* subtle borders                    */
#define PK_TEAL    0x2BD4C4   /* primary accent                    */
#define PK_BLUE    0x3BA0F0   /* iridescent mid                    */
#define PK_VIOLET  0x8E7BF0   /* iridescent end                    */
#define PK_TEALDK  0x123A36   /* pressed-state fill                */

#endif /* PRP_THEME_H */
