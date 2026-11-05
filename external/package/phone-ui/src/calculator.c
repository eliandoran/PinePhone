/*
 * Calculator: four functions on doubles, entered like a pocket calculator
 * (operators apply left to right, = repeats the last operation).
 *
 * The fonts only have ASCII and the LVGL symbols: the operator keys use the
 * plus, minus and close symbols, and draw division and equals from bars.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui.h"

#define KEY 150      /* key diameter */
#define GAP 24
#define DISPLAY_PAD 40
#define MAX_DIGITS 12

#define COLOR_FUNC 0xa5a5a5
#define COLOR_FUNC_PRESSED 0xd4d4d2
#define COLOR_OP UI_COLOR_ORANGE
#define COLOR_OP_PRESSED UI_COLOR_ORANGE_PRESSED

enum key_kind { DIGIT, FUNC, OP };

static struct {
	lv_obj_t *result, *expr, *clear_label;
	lv_obj_t *op_keys[4];

	double acc;           /* left operand, or the result */
	char op;              /* pending operator, 0: none */
	char last_op;         /* for repeated = */
	double last_operand;
	char entry[MAX_DIGITS + 4]; /* number being typed */
	bool entering;
	bool error;
} calc;

static const char ops[] = "/*-+";

static double apply(double a, char op, double b)
{
	switch (op) {
	case '+': return a + b;
	case '-': return a - b;
	case '*': return a * b;
	case '/': return b == 0 ? NAN : a / b;
	}
	return b;
}

static const char *op_text(char op)
{
	switch (op) {
	case '+': return "+";
	case '-': return "-";
	case '*': return "x";
	case '/': return "/";
	}
	return "";
}

/* `in` with thousands separators in its integer part */
static void group(char *out, size_t size, const char *in)
{
	const char *digits = in + (*in == '-');
	size_t int_len = strspn(digits, "0123456789");
	size_t o = 0;

	if (strchr(in, 'e') || strchr(in, 'n') || strchr(in, 'i')) {
		snprintf(out, size, "%s", in);
		return;
	}
	if (*in == '-' && o + 1 < size)
		out[o++] = '-';
	for (size_t i = 0; i < int_len && o + 2 < size; i++) {
		if (i && (int_len - i) % 3 == 0)
			out[o++] = ',';
		out[o++] = digits[i];
	}
	snprintf(out + o, size - o, "%s", digits + int_len);
}

static void format_number(char *out, size_t size, double v)
{
	char buf[48];
	if (v == 0)
		v = 0; /* no "-0" */
	snprintf(buf, sizeof(buf), "%.10g", v);
	group(out, size, buf);
}

static double current(void)
{
	return calc.entering ? strtod(calc.entry, NULL) : calc.acc;
}

/* The biggest font the text fits in, clipped beyond the smallest */
static void result_set(const char *text)
{
	static const lv_font_t *const fonts[] = {
		&ui_font_48, &ui_font_32, &ui_font_26,
	};
	int32_t width = lv_obj_get_content_width(calc.result);
	const lv_font_t *font = fonts[0];

	for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); i++) {
		lv_point_t size;
		font = fonts[i];
		lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		if (size.x <= width)
			break;
	}
	lv_obj_set_style_text_font(calc.result, font, 0);
	/* Changes with every key and has no bound: plain text */
	lv_label_set_text(calc.result, text);
}

static void op_highlight(void)
{
	for (int i = 0; i < 4; i++) {
		lv_obj_t *key = calc.op_keys[i];
		bool on = calc.op == ops[i] && !calc.entering;
		lv_color_t fg = on ? lv_color_hex(COLOR_OP) : lv_color_white();

		lv_obj_set_style_bg_color(key, on ? lv_color_white() : lv_color_hex(COLOR_OP), 0);
		for (uint32_t c = 0; c < lv_obj_get_child_count(key); c++) {
			lv_obj_t *part = lv_obj_get_child(key, (int32_t)c);
			if (lv_obj_check_type(part, &lv_label_class))
				lv_obj_set_style_text_color(part, fg, 0);
			else
				lv_obj_set_style_bg_color(part, fg, 0);
		}
	}
}

static void refresh(void)
{
	char text[64];

	if (calc.error) {
		result_set("Error");
	} else if (calc.entering) {
		group(text, sizeof(text), calc.entry);
		result_set(text);
	} else {
		format_number(text, sizeof(text), calc.acc);
		result_set(text);
	}

	bool clear_entry = calc.entering && strcmp(calc.entry, "0") != 0;
	const char *clear_text = clear_entry ? "C" : "AC";
	if (lv_label_get_text(calc.clear_label) != clear_text)
		lv_label_set_text_static(calc.clear_label, clear_text);
	op_highlight();
}

static void expr_set(double a, char op, const double *b)
{
	char x[48], y[48], text[128];
	format_number(x, sizeof(x), a);
	if (b) {
		format_number(y, sizeof(y), *b);
		snprintf(text, sizeof(text), "%s %s %s =", x, op_text(op), y);
	} else {
		snprintf(text, sizeof(text), "%s %s", x, op_text(op));
	}
	lv_label_set_text(calc.expr, text);
}

static void reset(void)
{
	calc.acc = 0;
	calc.op = calc.last_op = 0;
	calc.entering = false;
	calc.error = false;
	snprintf(calc.entry, sizeof(calc.entry), "0");
	lv_label_set_text_static(calc.expr, "");
}

/* Result of a computation; division by zero (NaN) or overflow is an error */
static void result(double v)
{
	if (!isfinite(v)) {
		reset();
		calc.error = true;
		return;
	}
	calc.acc = v;
}

static void digit(char d)
{
	if (calc.error)
		reset();
	if (!calc.entering) {
		snprintf(calc.entry, sizeof(calc.entry), "0");
		calc.entering = true;
		if (!calc.op) {
			/* A new calculation after = */
			calc.last_op = 0;
			lv_label_set_text_static(calc.expr, "");
		}
	}

	size_t len = strlen(calc.entry);
	size_t ndigits = strspn(calc.entry + (calc.entry[0] == '-'), "0123456789.") -
	                 (strchr(calc.entry, '.') != NULL);
	if (d == '.') {
		if (!strchr(calc.entry, '.'))
			snprintf(calc.entry + len, sizeof(calc.entry) - len, ".");
		return;
	}
	if (ndigits >= MAX_DIGITS)
		return;
	if (strcmp(calc.entry, "0") == 0 || strcmp(calc.entry, "-0") == 0)
		len--; /* replace the leading zero */
	calc.entry[len] = d;
	calc.entry[len + 1] = '\0';
}

static void operator(char op)
{
	if (calc.error)
		reset();
	if (calc.entering) {
		double v = strtod(calc.entry, NULL);
		calc.entering = false;
		if (calc.op)
			result(apply(calc.acc, calc.op, v));
		else
			calc.acc = v;
		if (calc.error)
			return;
	}
	calc.op = op;
	calc.last_op = 0;
	expr_set(calc.acc, op, NULL);
}

static void equals(void)
{
	if (calc.error)
		return;
	double a = calc.acc, b;
	char op;
	if (calc.op) {
		op = calc.op;
		b = current();
	} else if (calc.last_op) {
		op = calc.last_op;
		a = current();
		b = calc.last_operand;
	} else {
		return;
	}
	calc.entering = false;
	calc.op = 0;
	calc.last_op = op;
	calc.last_operand = b;
	result(apply(a, op, b));
	if (!calc.error)
		expr_set(a, op, &b);
}

static void clear(void)
{
	if (calc.entering && strcmp(calc.entry, "0") != 0)
		snprintf(calc.entry, sizeof(calc.entry), "0");
	else
		reset();
}

static void negate(void)
{
	if (calc.error)
		return;
	if (calc.entering) {
		if (calc.entry[0] == '-')
			memmove(calc.entry, calc.entry + 1, strlen(calc.entry));
		else if (strlen(calc.entry) + 1 < sizeof(calc.entry)) {
			memmove(calc.entry + 1, calc.entry, strlen(calc.entry) + 1);
			calc.entry[0] = '-';
		}
	} else {
		calc.acc = -calc.acc;
	}
}

/* Percent of the left operand when adding or subtracting (100 + 5% = 105) */
static void percent(void)
{
	if (calc.error)
		return;
	double v = current();
	v = calc.op == '+' || calc.op == '-' ? calc.acc * v / 100 : v / 100;
	char buf[32];
	snprintf(buf, sizeof(buf), "%.10g", v == 0 ? 0 : v);
	if (strchr(buf, 'e') || strlen(buf) >= sizeof(calc.entry)) {
		/* Too small or big to type: becomes the result instead */
		calc.entering = false;
		result(v);
		return;
	}
	memcpy(calc.entry, buf, strlen(buf) + 1);
	calc.entering = true;
}

static void key_pressed_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	ui_vibrate(UI_VIBRATE_TAP);
}

static void key_cb(lv_event_t *e)
{
	char key = (char)(uintptr_t)lv_event_get_user_data(e);

	switch (key) {
	case 'C': clear(); break;
	case 'n': negate(); break;
	case '%': percent(); break;
	case '=': equals(); break;
	case '+': case '-': case '*': case '/': operator(key); break;
	default: digit(key); break;
	}
	refresh();
}

/* A bar or dot of the drawn operators */
static void glyph_part(lv_obj_t *key, int32_t w, int32_t h, int32_t y)
{
	lv_obj_t *part = ui_box(key);
	lv_obj_set_size(part, w, h);
	lv_obj_set_style_radius(part, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(part, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(part, lv_color_white(), 0);
	lv_obj_align(part, LV_ALIGN_CENTER, 0, y);
}

static lv_obj_t *key_create(lv_obj_t *pad, char key, enum key_kind kind, int32_t width)
{
	const uint32_t bg[] = { UI_COLOR_BUTTON, COLOR_FUNC, COLOR_OP };
	const uint32_t pressed[] = { UI_COLOR_BUTTON_PRESSED, COLOR_FUNC_PRESSED, COLOR_OP_PRESSED };

	lv_obj_t *obj = lv_obj_create(pad);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, width, KEY);
	lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(bg[kind]), 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(pressed[kind]), LV_STATE_PRESSED);
	lv_obj_set_style_shadow_width(obj, 0, 0);
	lv_obj_set_style_text_color(obj, kind == FUNC ? lv_color_black() :
	                            kind == OP ? lv_color_white() : lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_clickable(obj, true);
	lv_obj_set_scrollable(obj, false);
	lv_obj_add_event_cb(obj, key_pressed_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(obj, key_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)key);

	const char *text = NULL;
	const lv_font_t *font = &ui_font_48;
	switch (key) {
	case '/':
		glyph_part(obj, 42, 9, 0);
		glyph_part(obj, 11, 11, -17);
		glyph_part(obj, 11, 11, 17);
		break;
	case '=':
		glyph_part(obj, 42, 9, -9);
		glyph_part(obj, 42, 9, 9);
		break;
	case '*': text = LV_SYMBOL_CLOSE; break;
	case '-': text = LV_SYMBOL_MINUS; break;
	case '+': text = LV_SYMBOL_PLUS; break;
	case 'C': text = "AC"; font = &ui_font_32; break;
	case 'n': text = "+/-"; font = &ui_font_32; break;
	case '%': text = "%"; font = &ui_font_32; break;
	case '0': text = "0"; break;
	case '1': text = "1"; break;
	case '2': text = "2"; break;
	case '3': text = "3"; break;
	case '4': text = "4"; break;
	case '5': text = "5"; break;
	case '6': text = "6"; break;
	case '7': text = "7"; break;
	case '8': text = "8"; break;
	case '9': text = "9"; break;
	case '.': text = "."; break;
	}
	if (text) {
		lv_obj_t *label = lv_label_create(obj);
		lv_label_set_text_static(label, text);
		lv_obj_set_style_text_font(label, font, 0);
		if (width > KEY) /* the wide 0 key: text where a round key would have it */
			lv_obj_align(label, LV_ALIGN_LEFT_MID, KEY / 2 - 14, 0);
		else
			lv_obj_center(label);
		if (key == 'C')
			calc.clear_label = label;
	}

	const char *op = strchr(ops, key);
	if (op && key)
		calc.op_keys[op - ops] = obj;
	return obj;
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	memset(&calc, 0, sizeof(calc));
}

void calculator_open(void)
{
	static const struct { char key; enum key_kind kind; } keys[] = {
		{ 'C', FUNC },  { 'n', FUNC },  { '%', FUNC },  { '/', OP },
		{ '7', DIGIT }, { '8', DIGIT }, { '9', DIGIT }, { '*', OP },
		{ '4', DIGIT }, { '5', DIGIT }, { '6', DIGIT }, { '-', OP },
		{ '1', DIGIT }, { '2', DIGIT }, { '3', DIGIT }, { '+', OP },
		{ '0', DIGIT }, { '.', DIGIT }, { '=', OP },
	};
	lv_obj_t *body;
	lv_obj_t *scr = ui_app_screen_create("Calculator", NULL, &body);
	lv_obj_set_scrollable(body, false);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);

	lv_obj_t *display = ui_box(body);
	lv_obj_set_width(display, LV_PCT(100));
	lv_obj_set_flex_grow(display, 1);
	lv_obj_set_flex_flow(display, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(display, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_hor(display, DISPLAY_PAD, 0);
	lv_obj_set_style_pad_bottom(display, 32, 0);
	lv_obj_set_style_pad_row(display, 12, 0);

	calc.expr = lv_label_create(display);
	lv_label_set_text_static(calc.expr, "");
	lv_obj_set_width(calc.expr, LV_PCT(100));
	lv_obj_set_style_text_font(calc.expr, &ui_font_32, 0);
	lv_obj_set_style_text_color(calc.expr, lv_color_hex(UI_COLOR_DIM), 0);
	lv_obj_set_style_text_align(calc.expr, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(calc.expr, LV_LABEL_LONG_MODE_CLIP);

	calc.result = lv_label_create(display);
	lv_label_set_text_static(calc.result, "0");
	lv_obj_set_width(calc.result, LV_PCT(100));
	lv_obj_set_style_text_font(calc.result, &ui_font_48, 0);
	lv_obj_set_style_text_align(calc.result, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(calc.result, LV_LABEL_LONG_MODE_CLIP);

	lv_obj_t *pad = ui_box(body);
	lv_obj_set_size(pad, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(pad, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(pad, GAP, 0);
	lv_obj_set_style_pad_row(pad, GAP, 0);
	lv_obj_set_style_pad_bottom(pad, 48, 0);

	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		key_create(pad, keys[i].key, keys[i].kind, keys[i].key == '0' ? 2 * KEY + GAP : KEY);

	reset();
	/* Sizes are known after the layout, for fitting the result */
	lv_obj_update_layout(scr);
	refresh();
	ui_push(scr);
}
