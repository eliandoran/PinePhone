/*
 * What the pieces of the shared UI (ui.c, nav.c, transition.c, gesture.c, status.c,
 * theme.c, wallpaper.c, battery_icon.c, usb_net.c) tell each other; not for the apps,
 * nor installed for plugins.
 */

#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include <time.h>

#include "ui.h"

/* The display's size now (ui.c), turned with it */
extern int32_t ui_screen_w, ui_screen_h;

/* Emoji by their names as GitHub writes them in Markdown (:tada:), sorted by name
 * (gen/shortcodes.c, gen-shortcodes.sh), for markdown.c */
struct shortcode {
	const char *name, *emoji;
};
extern const struct shortcode shortcodes[];
extern const unsigned shortcode_count;

/* What a monitor shows (monitor.c, main.c mirrors it): the area of the display, false
 * all of it */
bool ui_monitor_area(lv_area_t *area);

/* Theme (theme.c) */
void theme_apply(bool light);

/* Wallpaper (wallpaper.c): a screen's (ui_screen_create()) for the screen's size now */
void wallpaper_refit(lv_obj_t *scr);

/* Screens (nav.c) */
bool nav_chrome_hidden(void);          /* sideways, a full screen screen on top */
lv_obj_t *nav_back_target(void);       /* the screen below the top one, NULL at home */
lv_obj_t *nav_home_target(void);       /* the home screen, NULL when it is on top */
void nav_went_back(void);              /* a back gesture let go: as ui_pop() */
void nav_went_home(void);              /* a home gesture let go: as ui_home() */
void nav_rebuild(void (*reopen)(void)); /* the screens made again (theme, language) */
void nav_refit_wallpapers(void);

/*
 * Transitions (transition.c): `to` slid in over the screen shown, or under it. A drag
 * moves it by hand (gestures, the menu), let go it finishes from there.
 */
void transition_init(lv_display_t *disp); /* with LV_USE_3DTEXTURE, else LVGL's animations */
void transition_resize(void);
void transition_start(lv_obj_t *to, lv_screen_load_anim_t anim, bool del);
void transition_finish(void); /* one under way, done */
/* False while one runs or without them; else started, following transition_drag() */
bool transition_drag_begin(lv_obj_t *to, lv_screen_load_anim_t anim);
bool transition_dragging(void);
void transition_drag(int32_t pos); /* how far along, in px */
int32_t transition_pos(void);
/* Done: on to `to`, `from` deleted if `del`; else back to `from` */
void transition_drag_end(bool done, bool del);

/* The back and home gestures (gesture.c) */
void gestures_init(void);
void gestures_update(void); /* shown while there is a screen to go back from */
void gestures_recolor(void);
void gestures_resize(void);

/* Status bar (status.c) */
lv_obj_t *status_init(void); /* the bar, on the top layer */
void status_recolor(void);
void status_chrome(bool hidden, bool time_hidden);

/* Battery icon (battery_icon.c): the number in it or not; what it was */
bool battery_icon_percent(lv_obj_t *icon, bool on);

/* Internet over the USB cable (usb_net.c), from the status bar's updates */
void usb_net_update(time_t now);
bool usb_net_lit(void); /* the computer has its address on the USB network */

#endif
