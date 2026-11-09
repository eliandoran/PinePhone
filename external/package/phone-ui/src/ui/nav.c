/*
 * Screens and navigation: the stack of screens (the home screen first, the menu or
 * the apps' screens over it), going between them (transition.c slides them), and
 * the screens apps make (the wallpaper under them, a header with a back button).
 */

#include "ui_internal.h"

static lv_obj_t *stack[32]; /* stack[0]: home screen; Files pushes a screen per folder */
#define STACK_URGENT 4 /* kept for ui_push_now(): a call, an alert, however deep the rest */
static unsigned depth;
static lv_obj_t *menu_screen;
static bool menu_opening; /* the menu dragged up, else down (ui_menu_drag_begin()) */

/* Sideways, a full screen screen on top (ui_screen_turns()): no status bar, no back
 * gesture */
bool nav_chrome_hidden(void)
{
	return depth && ui_screen_fullscreen(stack[depth - 1]) && ui_sideways();
}

void ui_chrome_update(void)
{
	/* The home and lock screens show the time big: not twice */
	status_chrome(nav_chrome_hidden(), depth <= 1 || ui_locked());
	gestures_update();
}

lv_obj_t *nav_back_target(void)
{
	return depth > 1 ? stack[depth - 2] : NULL;
}

lv_obj_t *nav_home_target(void)
{
	return depth > 1 ? stack[0] : NULL;
}

void nav_went_back(void)
{
	depth--; /* as ui_pop() */
}

/* The screens between the home screen and the top one deleted, the top one left to
 * the transition going home */
void nav_went_home(void)
{
	for (unsigned i = 1; i < depth - 1; i++)
		if (stack[i] != menu_screen)
			lv_obj_delete_async(stack[i]);
	depth = 1;
}

void nav_refit_wallpapers(void)
{
	for (unsigned i = 0; i < depth; i++)
		wallpaper_refit(stack[i]);
	wallpaper_refit(menu_screen);
}

void ui_set_screens(lv_obj_t *home, lv_obj_t *menu)
{
	transition_finish(); /* the screens it shows are deleted next (theme change) */
	stack[0] = home;
	depth = 1;
	menu_screen = menu;
	lv_screen_load(home);
	ui_chrome_update();
}

void nav_rebuild(void (*reopen)(void))
{
	lv_obj_t *old[sizeof(stack) / sizeof(stack[0]) + 1];
	unsigned count = 0;
	bool menu_open = false;
	for (unsigned i = 0; i < depth; i++) {
		menu_open |= stack[i] == menu_screen;
		if (stack[i] != menu_screen)
			old[count++] = stack[i];
	}
	old[count++] = menu_screen;

	/* Without transitions: the new screens replace the old ones at once */
	uint32_t anim_ms = ui_anim_ms;
	ui_anim_ms = 0;
	home_create();
	if (menu_open)
		ui_open_menu();
	if (reopen)
		reopen();
	ui_anim_ms = anim_ms;

	for (unsigned i = 0; i < count; i++)
		lv_obj_delete(old[i]);
}

lv_obj_t *ui_screen_create(lv_opa_t dim, lv_obj_t **content)
{
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_scrollable(scr, false);

	lv_obj_t *img = lv_image_create(scr);
	lv_image_set_src(img, ui_wallpaper(dim));
	lv_obj_set_user_data(img, (void *)(uintptr_t)dim); /* for another when the display turns */

	lv_obj_t *c = lv_obj_create(scr);
	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_pad_top(c, UI_STATUS_H, 0);
	lv_obj_set_style_text_color(c, lv_color_hex(UI_COLOR_TEXT), 0);
	if (content)
		*content = c;
	return scr;
}

static void back_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_pop();
}

/* A header's title w wide: centered, or from its start if it does not fit (cut at
 * the end, not on both sides) */
static void title_fit(lv_obj_t *label, int32_t w)
{
	lv_point_t size;
	lv_text_get_size(&size, lv_label_get_text(label), lv_obj_get_style_text_font(label, 0), 0, 0,
	                 LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(label, w);
	lv_obj_set_style_text_align(label, size.x > w ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);
}

lv_obj_t *ui_app_screen_create(const char *title, lv_obj_t **header, lv_obj_t **body)
{
	lv_obj_t *content;
	lv_obj_t *scr = ui_screen_create(LV_OPA_70, &content);
	lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *h = lv_obj_create(content);
	lv_obj_remove_style_all(h);
	lv_obj_set_size(h, LV_PCT(100), UI_HEADER_H);

	lv_obj_t *back = lv_button_create(h);
	lv_obj_set_size(back, 80, 72);
	lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
	lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(back, 0, 0);
	lv_obj_set_style_text_color(back, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *icon = lv_label_create(back);
	lv_label_set_text_static(icon, LV_SYMBOL_LEFT);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);

	lv_obj_t *label = lv_label_create(h);
	lv_label_set_text_static(label, title);
	lv_obj_set_style_text_font(label, UI_FONT_HEADLINE, 0);
	title_fit(label, ui_screen_w - 2 * 104);
	lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

	lv_obj_t *b = lv_obj_create(content);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LV_PCT(100));
	lv_obj_set_flex_grow(b, 1);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);

	if (header)
		*header = h;
	*body = b;
	return scr;
}

void ui_screen_set_title(lv_obj_t *header, const char *title)
{
	lv_obj_t *label = lv_obj_get_child(header, 1); /* after the back button */
	lv_label_set_text(label, title);
	title_fit(label, ui_screen_w - 2 * 104);
}

lv_obj_t *ui_header_button(lv_obj_t *header, const char *symbol, lv_event_cb_t cb, void *user_data)
{
	/* Further buttons go left of the previous ones */
	int32_t n = (int32_t)lv_obj_get_child_count(header) - 2;

	lv_obj_t *button = lv_button_create(header);
	lv_obj_set_size(button, 80, 72);
	lv_obj_align(button, LV_ALIGN_RIGHT_MID, -12 - 88 * n, 0);
	lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
	lv_obj_set_style_shadow_width(button, 0, 0);
	lv_obj_set_style_text_color(button, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, user_data);
	lv_obj_t *icon = lv_label_create(button);
	lv_label_set_text_static(icon, symbol);
	lv_obj_set_style_text_font(icon, &ui_font_32, 0);
	lv_obj_center(icon);
	/* The title (centered, after the back button) clear of them */
	if (n > 0)
		title_fit(lv_obj_get_child(header, 1), ui_screen_w - 2 * (104 + 88 * n));
	return button;
}

bool ui_at_home(void)
{
	return depth <= 1 || stack[depth - 1] == menu_screen;
}

bool nav_menu_on_top(void)
{
	return depth > 1 && stack[depth - 1] == menu_screen;
}

lv_obj_t *ui_current(void)
{
	return stack[depth - 1];
}

void ui_push(lv_obj_t *screen)
{
	if (depth >= sizeof(stack) / sizeof(stack[0]) - STACK_URGENT) {
		lv_obj_delete(screen);
		return;
	}
	stack[depth++] = screen;
	transition_start(screen, LV_SCREEN_LOAD_ANIM_MOVE_LEFT, false);
}

void ui_push_now(lv_obj_t *screen)
{
	if (depth == sizeof(stack) / sizeof(stack[0])) {
		lv_obj_delete(screen);
		return;
	}
	stack[depth++] = screen;
	ui_turn_for(screen);
	transition_finish(); /* one under way, done */
	ui_chrome_update();
	lv_screen_load(screen);
}

void ui_open_menu(void)
{
	stack[depth++] = menu_screen;
	transition_start(menu_screen, LV_SCREEN_LOAD_ANIM_MOVE_TOP, false);
}

/* The menu dragged up from the home screen, or down from the menu: the slide follows
 * the finger, as for the back gesture; once let go, it finishes from there */
bool ui_menu_drag_begin(bool open)
{
	if (open ? depth != 1 : (depth != 2 || stack[1] != menu_screen))
		return false;
	if (!transition_drag_begin(open ? menu_screen : stack[0],
	                           open ? LV_SCREEN_LOAD_ANIM_MOVE_TOP : LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM))
		return false;
	menu_opening = open;
	return true;
}

void ui_menu_drag(int32_t pos)
{
	if (transition_dragging())
		transition_drag(pos);
}

void ui_menu_drag_end(bool done)
{
	if (!transition_dragging())
		return;
	transition_drag_end(done, false);
	if (done && menu_opening)
		stack[depth++] = menu_screen; /* as ui_open_menu() */
	else if (done)
		depth--; /* as ui_pop() */
	ui_chrome_update();
}

void ui_pop(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[--depth];
	if (top == menu_screen)
		transition_start(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, false);
	else
		transition_start(stack[depth - 1], LV_SCREEN_LOAD_ANIM_MOVE_RIGHT, true);
}

void ui_home(void)
{
	if (depth <= 1)
		return;
	lv_obj_t *top = stack[depth - 1];
	nav_went_home();
	if (top == menu_screen)
		transition_start(stack[0], LV_SCREEN_LOAD_ANIM_MOVE_BOTTOM, false);
	else
		transition_start(stack[0], LV_SCREEN_LOAD_ANIM_OUT_TOP, true); /* as the home gesture */
}
