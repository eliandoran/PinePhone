/*
 * Snake: steer with swipes anywhere on the screen, eat to grow, walls and the
 * snake itself end the game. It speeds up with every piece eaten. The best score
 * is a setting (snake_best).
 *
 * Each segment is an object moved on the board: a step moves the tail's object to
 * the new head (or adds one when eating), so only two cells are redrawn per step.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ui.h"

#define COLS 17
#define ROWS 28
#define CELL 40
#define BOARD_PAD 8
#define SEG 36           /* segment size, CELL minus a gap */
#define FOOD 30
#define SWIPE 48         /* finger travel that turns the snake */
#define STEP_MS 150      /* first speed */
#define STEP_MIN_MS 70
#define STEP_FASTER_MS 3 /* per piece eaten */
#define REPLAY_MS 600    /* taps ignored after a game over */

#define COLOR_SNAKE UI_COLOR_GREEN
#define COLOR_HEAD 0x1f9e40
#define COLOR_FOOD UI_COLOR_RED

enum state { READY, RUNNING, PAUSED, OVER };

struct cell { int8_t x, y; };

static const struct cell dirs[4] = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } };
enum { RIGHT, DOWN, LEFT, UP };

static struct {
	lv_obj_t *board, *food, *message, *score, *best, *pause_icon;
	lv_timer_t *timer;

	enum state state;
	/* Ring buffer of the snake, tail to head */
	struct cell body[COLS * ROWS];
	lv_obj_t *segs[COLS * ROWS];
	unsigned head, len;
	bool occupied[ROWS][COLS];
	struct cell food_at;

	int dir;
	int queue[2]; /* turns swiped ahead of the next steps */
	unsigned queued;
	int score_n, best_n;

	lv_point_t drag; /* finger travel since the last turn */
	bool swiped;
	uint32_t over_tick;
} snake;

static lv_obj_t *cell_create(int32_t size, uint32_t color, int32_t radius)
{
	lv_obj_t *obj = lv_obj_create(snake.board);
	lv_obj_remove_style_all(obj);
	lv_obj_set_size(obj, size, size);
	lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
	lv_obj_set_style_radius(obj, radius, 0);
	lv_obj_set_clickable(obj, false);
	return obj;
}

static void place(lv_obj_t *obj, struct cell c, int32_t size)
{
	int32_t off = (CELL - size) / 2;
	lv_obj_set_pos(obj, c.x * CELL + off, c.y * CELL + off);
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
	ui_set_text_fmt(snake.score, "Score %d", snake.score_n);
	ui_set_text_fmt(snake.best, "Best %d", snake.best_n);
}

static void food_place(void)
{
	unsigned free_cells = COLS * ROWS - snake.len;
	unsigned n = (unsigned)rand() % free_cells;

	for (int y = 0; y < ROWS; y++)
		for (int x = 0; x < COLS; x++)
			if (!snake.occupied[y][x] && n-- == 0) {
				snake.food_at = (struct cell){ (int8_t)x, (int8_t)y };
				place(snake.food, snake.food_at, FOOD);
				return;
			}
}

static void reset(void)
{
	for (unsigned i = 0; i < snake.len; i++) {
		unsigned slot = (snake.head + COLS * ROWS - i) % (COLS * ROWS);
		lv_obj_delete(snake.segs[slot]);
		snake.segs[slot] = NULL;
	}
	memset(snake.occupied, 0, sizeof(snake.occupied));

	snake.len = 3;
	snake.head = snake.len - 1;
	for (unsigned i = 0; i < snake.len; i++) {
		struct cell c = { (int8_t)(COLS / 2 - 2 + i), ROWS / 2 };
		snake.body[i] = c;
		snake.occupied[c.y][c.x] = true;
		snake.segs[i] = cell_create(SEG, i == snake.head ? COLOR_HEAD : COLOR_SNAKE, 8);
		place(snake.segs[i], c, SEG);
	}
	snake.dir = RIGHT;
	snake.queued = 0;
	snake.score_n = 0;
	score_show();
	food_place();
	lv_timer_set_period(snake.timer, STEP_MS);
	snake.state = READY;
	message_show("Swipe to start");
}

static void game_over(const char *text)
{
	snake.state = OVER;
	snake.over_tick = lv_tick_get();
	lv_timer_pause(snake.timer);
	ui_vibrate(60);
	if (snake.score_n > snake.best_n) {
		snake.best_n = snake.score_n;
		ui_setting_set_int("snake_best", snake.best_n);
		score_show();
	}
	message_show(text);
}

static void step_cb(lv_timer_t *timer)
{
	LV_UNUSED(timer);
	if (snake.queued) {
		snake.dir = snake.queue[0];
		snake.queue[0] = snake.queue[1];
		snake.queued--;
	}

	const unsigned cap = COLS * ROWS;
	struct cell h = snake.body[snake.head];
	struct cell n = { (int8_t)(h.x + dirs[snake.dir].x), (int8_t)(h.y + dirs[snake.dir].y) };
	if (n.x < 0 || n.x >= COLS || n.y < 0 || n.y >= ROWS) {
		game_over("Game over\nTap to play again");
		return;
	}

	bool eat = n.x == snake.food_at.x && n.y == snake.food_at.y;
	unsigned tail = (snake.head + cap - snake.len + 1) % cap;
	if (!eat) /* the tail moves away: the head may take its cell */
		snake.occupied[snake.body[tail].y][snake.body[tail].x] = false;
	if (snake.occupied[n.y][n.x]) {
		game_over("Game over\nTap to play again");
		return;
	}

	lv_obj_set_style_bg_color(snake.segs[snake.head], lv_color_hex(COLOR_SNAKE), 0);
	unsigned next = (snake.head + 1) % cap;
	lv_obj_t *seg;
	if (eat) {
		seg = cell_create(SEG, COLOR_HEAD, 8);
		snake.len++;
	} else {
		seg = snake.segs[tail];
		snake.segs[tail] = NULL;
		lv_obj_set_style_bg_color(seg, lv_color_hex(COLOR_HEAD), 0);
	}
	snake.head = next;
	snake.body[next] = n;
	snake.segs[next] = seg;
	snake.occupied[n.y][n.x] = true;
	place(seg, n, SEG);

	if (eat) {
		ui_vibrate(15);
		snake.score_n++;
		score_show();
		if (snake.len == cap) {
			lv_obj_set_hidden(snake.food, true);
			game_over("You win!\nTap to play again");
			return;
		}
		food_place();
		int ms = STEP_MS - STEP_FASTER_MS * snake.score_n;
		lv_timer_set_period(snake.timer, ms < STEP_MIN_MS ? STEP_MIN_MS : (uint32_t)ms);
	}
}

static void run(void)
{
	snake.state = RUNNING;
	message_show(NULL);
	lv_obj_set_hidden(snake.food, false);
	lv_label_set_text_static(snake.pause_icon, LV_SYMBOL_PAUSE);
	lv_timer_resume(snake.timer);
	lv_timer_reset(snake.timer);
}

static void pause_game(void)
{
	snake.state = PAUSED;
	lv_timer_pause(snake.timer);
	lv_label_set_text_static(snake.pause_icon, LV_SYMBOL_PLAY);
	message_show("Paused\nTap to continue");
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
	if (snake.timer)
		lv_timer_delete(snake.timer);
	memset(&snake, 0, sizeof(snake));
}

void snake_open(void)
{
	lv_obj_t *header, *body;
	lv_obj_t *scr = ui_app_screen_create("Snake", &header, &body);
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
	lv_obj_set_style_text_font(snake.score, &ui_font_32, 0);
	lv_obj_set_style_text_color(snake.score, lv_color_hex(UI_COLOR_TEXT), 0);
	snake.best = ui_dim_label(scores, "", &ui_font_26);

	snake.board = ui_box(body);
	lv_obj_set_size(snake.board, COLS * CELL + 2 * BOARD_PAD, ROWS * CELL + 2 * BOARD_PAD);
	lv_obj_set_style_pad_all(snake.board, BOARD_PAD, 0);
	lv_obj_set_style_radius(snake.board, 24, 0);
	lv_obj_set_style_bg_opa(snake.board, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(snake.board, lv_color_hex(UI_COLOR_CARD), 0);

	snake.food = cell_create(FOOD, COLOR_FOOD, LV_RADIUS_CIRCLE);

	snake.message = lv_label_create(snake.board);
	lv_obj_set_style_text_align(snake.message, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_font(snake.message, &ui_font_32, 0);
	lv_obj_set_style_text_color(snake.message, lv_color_hex(UI_COLOR_TEXT), 0);
	lv_obj_set_style_bg_opa(snake.message, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(snake.message, lv_color_hex(UI_COLOR_BUTTON), 0);
	lv_obj_set_style_radius(snake.message, 24, 0);
	lv_obj_set_style_pad_hor(snake.message, 32, 0);
	lv_obj_set_style_pad_ver(snake.message, 24, 0);
	lv_obj_set_style_text_line_space(snake.message, 8, 0);
	lv_obj_align(snake.message, LV_ALIGN_CENTER, 0, -CELL * 3);

	snake.timer = lv_timer_create(step_cb, STEP_MS, NULL);
	lv_timer_pause(snake.timer);
	srand((unsigned)time(NULL));
	snake.best_n = ui_setting_get_int("snake_best", 0);
	reset();

	ui_push(scr);
}
