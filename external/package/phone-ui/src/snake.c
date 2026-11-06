/*
 * Snake: steer with swipes anywhere on the screen, eat to grow, walls and the
 * snake itself end the game. It speeds up with every piece eaten. The best score
 * is a setting (snake_best).
 *
 * The game moves cell by cell; the drawing slides between cells during each step
 * (an animation as long as the step, so in time with the display). The body is a
 * chain of links, rounded bars from one cell to the next: a step grows the link
 * into the new head and shrinks the tail's link, the others stay where they are,
 * so a frame only redraws around the head and the tail.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "i18n.h"
#include "ui.h"

#define COLS 17
#define ROWS 28
#define CELL 40
#define CAP (COLS * ROWS)
#define BOARD_PAD 8
#define SEG 32           /* snake width, CELL minus the gap between coils */
#define FOOD 30
#define SWIPE 48         /* finger travel that turns the snake */
#define STEP_MS 150      /* first speed */
#define STEP_MIN_MS 70
#define STEP_FASTER_MS 3 /* per piece eaten */
#define REPLAY_MS 600    /* taps ignored after a game over */
#define DONE 1024        /* progress of a step, 0 to DONE */

#define COLOR_SNAKE UI_COLOR_GREEN
#define COLOR_HEAD 0x1f9e40
#define COLOR_FOOD UI_COLOR_RED

enum state { READY, RUNNING, PAUSED, OVER };

struct cell { int8_t x, y; };

static const struct cell dirs[4] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
enum { RIGHT, DOWN, LEFT, UP };

static struct {
	lv_obj_t *board, *food, *message, *score, *best, *pause_icon;

	enum state state;
	/* Ring buffer of the snake, tail to head */
	struct cell body[CAP];
	lv_obj_t *links[CAP]; /* links[i]: from body[i] to the next cell */
	unsigned head, len;
	bool occupied[ROWS][COLS];
	struct cell food_at;

	lv_obj_t *head_obj;
	lv_obj_t *retract;    /* the link the tail leaves during this step, or NULL */
	struct cell retract_from;
	lv_obj_t *spare;      /* the previous step's retracted link, reused */
	int32_t progress;
	uint32_t step_ms;

	int dir;
	int queue[2]; /* turns swiped ahead of the next steps */
	unsigned queued;
	int score_n, best_n;

	lv_point_t drag; /* finger travel since the last turn */
	bool swiped;
	uint32_t over_tick;
} snake;

static lv_obj_t *shape_create(int32_t size, uint32_t color)
{
	lv_obj_t *obj = lv_obj_create(snake.board);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, size, size);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
	lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_clickable(obj, false);
	return obj;
}

static lv_obj_t *link_create(void)
{
	lv_obj_t *link = shape_create(SEG, COLOR_SNAKE);
	lv_obj_move_foreground(snake.head_obj); /* the head stays on top */
	return link;
}

/* Top left corner of the snake in a cell */
static int32_t px(int c)
{
	return c * CELL + (CELL - SEG) / 2;
}

/* A link from (x1, y1) to (x2, y2), the corners of the cells it joins */
static void link_set(lv_obj_t *link, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
	lv_obj_set_pos(link, LV_MIN(x1, x2), LV_MIN(y1, y2));
	lv_obj_set_size(link, LV_ABS(x2 - x1) + SEG, LV_ABS(y2 - y1) + SEG);
}

static unsigned tail_slot(void)
{
	return (snake.head + CAP - snake.len + 1) % CAP;
}

/* The head and the tail `p` of the way through the step (0 to DONE) */
static void render(int32_t p)
{
	unsigned prev = (snake.head + CAP - 1) % CAP;
	struct cell a = snake.body[prev], b = snake.body[snake.head];
	int32_t hx = px(a.x) + (px(b.x) - px(a.x)) * p / DONE;
	int32_t hy = px(a.y) + (px(b.y) - px(a.y)) * p / DONE;
	link_set(snake.links[prev], px(a.x), px(a.y), hx, hy);
	lv_obj_set_pos(snake.head_obj, hx, hy);

	if (snake.retract) {
		struct cell f = snake.retract_from, t = snake.body[tail_slot()];
		link_set(snake.retract, px(f.x) + (px(t.x) - px(f.x)) * p / DONE,
		         px(f.y) + (px(t.y) - px(f.y)) * p / DONE, px(t.x), px(t.y));
	}
}

static void message_show(const char *text)
{
	if (text) {
		lv_label_set_text_static(snake.message, text);
		lv_obj_set_hidden(snake.message, false);
		lv_obj_move_foreground(snake.message);
	} else {
		lv_obj_set_hidden(snake.message, true);
	}
}

static void score_show(void)
{
	ui_set_text_fmt(snake.score, _("Score %d"), snake.score_n);
	ui_set_text_fmt(snake.best, _("Best %d"), snake.best_n);
}

static void food_place(void)
{
	unsigned free_cells = CAP - snake.len;
	unsigned n = (unsigned)rand() % free_cells;

	for (int y = 0; y < ROWS; y++)
		for (int x = 0; x < COLS; x++)
			if (!snake.occupied[y][x] && n-- == 0) {
				snake.food_at = (struct cell){ (int8_t)x, (int8_t)y };
				lv_obj_set_pos(snake.food, x * CELL + (CELL - FOOD) / 2,
				               y * CELL + (CELL - FOOD) / 2);
				return;
			}
}

static void step(void);

static void anim_cb(void *var, int32_t v)
{
	LV_UNUSED(var);
	snake.progress = v;
	render(v);
}

static void step_done_cb(lv_anim_t *a)
{
	LV_UNUSED(a);
	step();
}

/* Runs the step from snake.progress to its end */
static void anim_start(void)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, &snake);
	lv_anim_set_exec_cb(&a, anim_cb);
	lv_anim_set_values(&a, snake.progress, DONE);
	lv_anim_set_duration(&a, snake.step_ms * (uint32_t)(DONE - snake.progress) / DONE);
	lv_anim_set_completed_cb(&a, step_done_cb);
	lv_anim_start(&a);
}

static void reset(void)
{
	lv_anim_delete(&snake, anim_cb);
	for (unsigned i = 1; i < snake.len; i++) {
		unsigned slot = (snake.head + CAP - i) % CAP;
		lv_obj_delete(snake.links[slot]);
		snake.links[slot] = NULL;
	}
	if (snake.retract)
		lv_obj_delete(snake.retract);
	if (snake.spare)
		lv_obj_delete(snake.spare);
	snake.retract = snake.spare = NULL;
	memset(snake.occupied, 0, sizeof(snake.occupied));

	snake.len = 3;
	snake.head = snake.len - 1;
	for (unsigned i = 0; i < snake.len; i++) {
		struct cell c = { (int8_t)(COLS / 2 - 2 + i), ROWS / 2 };
		snake.body[i] = c;
		snake.occupied[c.y][c.x] = true;
		if (i < snake.head) {
			snake.links[i] = link_create();
			link_set(snake.links[i], px(c.x), px(c.y), px(c.x + 1), px(c.y));
		}
	}
	snake.progress = DONE;
	render(DONE);
	snake.dir = RIGHT;
	snake.queued = 0;
	snake.score_n = 0;
	score_show();
	food_place();
	snake.step_ms = STEP_MS;
	snake.state = READY;
	message_show(_("Swipe to start"));
}

static void game_over(const char *text)
{
	snake.state = OVER;
	snake.over_tick = lv_tick_get();
	ui_vibrate(60);
	if (snake.score_n > snake.best_n) {
		snake.best_n = snake.score_n;
		ui_setting_set_int("snake_best", snake.best_n);
		score_show();
	}
	message_show(text);
}

/* The head has reached its cell: on to the next one */
static void step(void)
{
	if (snake.queued) {
		snake.dir = snake.queue[0];
		snake.queue[0] = snake.queue[1];
		snake.queued--;
	}

	struct cell h = snake.body[snake.head];
	struct cell n = { (int8_t)(h.x + dirs[snake.dir].x), (int8_t)(h.y + dirs[snake.dir].y) };
	if (n.x < 0 || n.x >= COLS || n.y < 0 || n.y >= ROWS) {
		game_over(_("Game over\nTap to play again"));
		return;
	}

	bool eat = n.x == snake.food_at.x && n.y == snake.food_at.y;
	unsigned tail = tail_slot();
	if (!eat) /* the tail moves away: the head may take its cell */
		snake.occupied[snake.body[tail].y][snake.body[tail].x] = false;
	if (snake.occupied[n.y][n.x]) {
		game_over(_("Game over\nTap to play again"));
		return;
	}

	/* The link that finished retracting grows into the new head */
	if (snake.retract) {
		snake.spare = snake.retract;
		snake.retract = NULL;
	}
	lv_obj_t *link = snake.spare ? snake.spare : link_create();
	snake.spare = NULL;
	if (eat) {
		snake.len++;
	} else {
		snake.retract = snake.links[tail];
		snake.retract_from = snake.body[tail];
		snake.links[tail] = NULL;
	}
	snake.links[snake.head] = link;
	snake.head = (snake.head + 1) % CAP;
	snake.body[snake.head] = n;
	snake.occupied[n.y][n.x] = true;

	if (eat) {
		ui_vibrate(UI_VIBRATE_TAP);
		snake.score_n++;
		score_show();
		if (snake.len == CAP) {
			render(DONE);
			lv_obj_set_hidden(snake.food, true);
			game_over(_("You win!\nTap to play again"));
			return;
		}
		food_place();
		int ms = STEP_MS - STEP_FASTER_MS * snake.score_n;
		snake.step_ms = ms < STEP_MIN_MS ? STEP_MIN_MS : (uint32_t)ms;
	}
	snake.progress = 0;
	render(0);
	anim_start();
}

static void run(void)
{
	bool fresh = snake.state == READY;
	snake.state = RUNNING;
	message_show(NULL);
	lv_obj_set_hidden(snake.food, false);
	lv_label_set_text_static(snake.pause_icon, LV_SYMBOL_PAUSE);
	if (fresh)
		step();
	else
		anim_start();
}

static void pause_game(void)
{
	snake.state = PAUSED;
	lv_anim_delete(&snake, anim_cb);
	lv_label_set_text_static(snake.pause_icon, LV_SYMBOL_PLAY);
	message_show(_("Paused\nTap to continue"));
}

/* A turn after the last one queued (or the current direction) */
static void turn(int d)
{
	int last = snake.queued ? snake.queue[snake.queued - 1] : snake.dir;
	if (d == last || d == (last + 2) % 4 || snake.queued == 2)
		return;
	snake.queue[snake.queued++] = d;
}

static void touch_cb(lv_event_t *e)
{
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		snake.drag = (lv_point_t){ 0, 0 };
		snake.swiped = false;
		break;
	case LV_EVENT_PRESSING: {
		lv_point_t v;
		lv_indev_get_vect(lv_indev_active(), &v);
		snake.drag.x += v.x;
		snake.drag.y += v.y;
		int32_t ax = LV_ABS(snake.drag.x), ay = LV_ABS(snake.drag.y);
		if (LV_MAX(ax, ay) < SWIPE)
			break;
		int d = ax > ay ? (snake.drag.x > 0 ? RIGHT : LEFT) : (snake.drag.y > 0 ? DOWN : UP);
		snake.drag = (lv_point_t){ 0, 0 };
		snake.swiped = true;
		if (snake.state == READY) {
			if (d != LEFT) /* the snake starts heading right */
				turn(d);
			run();
		} else if (snake.state == RUNNING) {
			turn(d);
		}
		break;
	}
	case LV_EVENT_CLICKED:
		if (snake.swiped)
			break;
		if (snake.state == READY || snake.state == PAUSED) {
			run();
		} else if (snake.state == OVER && lv_tick_elaps(snake.over_tick) > REPLAY_MS) {
			reset();
			run();
		}
		break;
	default:
		break;
	}
}

static void pause_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (snake.state == RUNNING)
		pause_game();
	else if (snake.state == PAUSED || snake.state == READY)
		run();
}

static void unload_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	if (snake.state == RUNNING)
		pause_game();
}

static void delete_cb(lv_event_t *e)
{
	LV_UNUSED(e);
	lv_anim_delete(&snake, anim_cb);
	memset(&snake, 0, sizeof(snake));
}

void snake_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create(_("Snake"), &header, &body);
	lv_obj_add_event_cb(scr, delete_cb, LV_EVENT_DELETE, NULL);
	lv_obj_add_event_cb(scr, unload_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
	lv_obj_t *pause_button = ui_header_button(header, LV_SYMBOL_PLAY, pause_cb, NULL);
	snake.pause_icon = lv_obj_get_child(pause_button, 0);

	/* The whole body takes the swipes, the board in it does not */
	lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_scrollable(body, false);
	lv_obj_set_clickable(body, true);
	lv_obj_add_event_cb(body, touch_cb, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(body, touch_cb, LV_EVENT_PRESSING, NULL);
	lv_obj_add_event_cb(body, touch_cb, LV_EVENT_CLICKED, NULL);

	lv_obj_t *scores = ui_box(body);
	lv_obj_set_size(scores, COLS * CELL + 2 * BOARD_PAD, 72);
	lv_obj_set_flex_flow(scores, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(scores, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(scores, 12, 0);
	snake.score = lv_label_create(scores);
	lv_obj_set_style_text_font(snake.score, UI_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(snake.score, lv_color_hex(UI_COLOR_TEXT), 0);
	snake.best = ui_dim_label(scores, "", UI_FONT_TITLE);

	snake.board = ui_box(body);
	lv_obj_set_size(snake.board, COLS * CELL + 2 * BOARD_PAD, ROWS * CELL + 2 * BOARD_PAD);
	lv_obj_set_style_pad_all(snake.board, BOARD_PAD, 0);
	lv_obj_set_style_radius(snake.board, 24, 0);
	lv_obj_set_style_bg_opa(snake.board, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(snake.board, lv_color_hex(UI_COLOR_CARD), 0);

	snake.food = shape_create(FOOD, COLOR_FOOD);
	snake.head_obj = shape_create(SEG, COLOR_HEAD);

	snake.message = lv_label_create(snake.board);
	lv_obj_set_style_text_align(snake.message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(snake.message, UI_FONT_HEADLINE, 0);
	lv_obj_set_style_text_color(snake.message, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(snake.message, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(snake.message, lv_color_hex(UI_COLOR_BUTTON), 0);
	lv_obj_set_style_radius(snake.message, 24, 0);
	lv_obj_set_style_pad_hor(snake.message, 32, 0);
	lv_obj_set_style_pad_ver(snake.message, 24, 0);
	lv_obj_set_style_text_line_space(snake.message, 8, 0);
	lv_obj_align(snake.message, LV_ALIGN_CENTER, 0, -CELL * 3);

	srand((unsigned)time(NULL));
	snake.best_n = ui_setting_get_int("snake_best", 0);
	reset();

	ui_push(scr);
}
