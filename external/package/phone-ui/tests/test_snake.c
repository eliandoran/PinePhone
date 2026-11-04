/* Snake (snake.c), through the UI: phone-ui on the host (ui_test.h). The food is
 * placed at random (seeded with the wall clock): a test that eats finds it on the
 * board and steers there. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define CELL 40  /* snake.c's */
#define FOOD 30
#define SEG 32
#define START_X 8 /* the head, heading right */
#define START_Y 14
#define OVER "Game over\nTap to play again"

TEST_SETUP
{
	ui_test_reset();
}

/* The finger without ui_test_settle(), which waits for animations to end: the
 * snake's steps are one after the other */
static void finger(int32_t x, int32_t y, int32_t dx, int32_t dy)
{
	lv_test_mouse_move_to(x, y);
	lv_test_mouse_press();
	ui_test_wait(16);
	if (dx || dy) {
		lv_test_mouse_move_to(x + dx, y + dy);
		ui_test_wait(16);
	} else {
		ui_test_wait(32);
	}
	lv_test_mouse_release();
	ui_test_wait(16);
}

/* Swipes 100 px on the screen */
static void swipe(int dx, int dy)
{
	finger(360, 900, dx, dy);
}

static void tap(void)
{
	finger(360, 900, 0, 0);
}

/* The header's button */
static bool tap_button(const char *symbol)
{
	lv_obj_t *label = ui_test_find(symbol);
	if (!label)
		return false;
	lv_area_t a;
	lv_obj_get_coords(label, &a);
	finger((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 0, 0);
	return true;
}

/* The board: what the message is in */
static lv_obj_t *board(void)
{
	lv_obj_t *m = ui_test_find("Swipe to start");
	return m ? lv_obj_get_parent(m) : NULL;
}

/* The food's cell */
static bool food(lv_obj_t *b, int *x, int *y)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(b); i++) {
		lv_obj_t *o = lv_obj_get_child(b, (int32_t)i);
		if (lv_obj_get_style_width(o, 0) == FOOD) {
			*x = (lv_obj_get_style_x(o, 0) - (CELL - FOOD) / 2) / CELL;
			*y = (lv_obj_get_style_y(o, 0) - (CELL - FOOD) / 2) / CELL;
			return true;
		}
	}
	return false;
}

/* The head's position in px, on its way to a cell */
static lv_obj_t *head(lv_obj_t *b)
{
	if (!b)
		return NULL;
	for (uint32_t i = 0; i < lv_obj_get_child_count(b); i++) {
		lv_obj_t *o = lv_obj_get_child(b, (int32_t)i);
		if (lv_color_eq(lv_obj_get_style_bg_color(o, 0), lv_color_hex(0x1f9e40)))
			return o;
	}
	return NULL;
}

TEST(ready)
{
	CHECK(ui_test_open("Snake"));
	CHECK(ui_test_shows("Swipe to start"));
	CHECK(ui_test_shows("Score 0"));
	CHECK(ui_test_shows("Best 0"));
}

TEST(best_score_kept)
{
	ui_test_write(SETTINGS, "snake_best=7\n");
	CHECK(ui_test_open("Snake"));
	CHECK(ui_test_shows("Best 7"));
}

/* Started with a tap, right into the wall */
TEST(wall)
{
	CHECK(ui_test_open("Snake"));
	tap();
	CHECK(!ui_test_shows("Swipe to start"));
	CHECK(ui_test_shows(LV_SYMBOL_PAUSE));
	for (int t = 0; t < 3000 && !ui_test_shows(OVER); t += 16)
		ui_test_wait(16);
	CHECK(ui_test_shows(OVER));

	/* A tap right after does not start again (it may be the last swipe's) */
	tap();
	CHECK(ui_test_shows(OVER));
	ui_test_wait(600);
	tap();
	CHECK(!ui_test_shows(OVER));
	CHECK(ui_test_shows("Score 0"));
	ui_test_wait(3000);
	CHECK(ui_test_shows(OVER));
}

/* Started with a swipe up: into the top */
TEST(swipe_up)
{
	CHECK(ui_test_open("Snake"));
	lv_obj_t *b = board();
	lv_obj_t *h = head(b);
	CHECK(b && h);
	swipe(0, -100);
	ui_test_wait(600);
	CHECK(lv_obj_get_style_x(h, 0) == START_X * CELL + (CELL - SEG) / 2);
	CHECK(lv_obj_get_style_y(h, 0) < START_Y * CELL);
	ui_test_wait(3000);
	CHECK(ui_test_shows(OVER));
}

/* Back on itself is not a turn; a swipe left to start goes right */
TEST(no_reversing)
{
	CHECK(ui_test_open("Snake"));
	lv_obj_t *h = head(board());
	CHECK(h != NULL);
	swipe(-100, 0);
	ui_test_wait(400);
	swipe(-100, 0);
	ui_test_wait(400);
	CHECK(lv_obj_get_style_x(h, 0) > START_X * CELL);
	CHECK(lv_obj_get_style_y(h, 0) == START_Y * CELL + (CELL - SEG) / 2);
}

/* Two turns queued within a step: down then left, a U-turn */
TEST(turns_queued)
{
	CHECK(ui_test_open("Snake"));
	lv_obj_t *h = head(board());
	CHECK(h != NULL);
	tap();
	swipe(0, 100);
	swipe(-100, 0);
	swipe(0, -100); /* a third one is dropped */
	ui_test_wait(1000);
	CHECK(lv_obj_get_style_x(h, 0) < START_X * CELL);
	CHECK(lv_obj_get_style_y(h, 0) > START_Y * CELL);
}

TEST(pause)
{
	CHECK(ui_test_open("Snake"));
	lv_obj_t *h = head(board());
	CHECK(h != NULL);
	if (!h)
		return;
	/* The header's button starts it too */
	CHECK(tap_button(LV_SYMBOL_PLAY));
	CHECK(!ui_test_shows("Swipe to start"));
	CHECK(tap_button(LV_SYMBOL_PAUSE));
	CHECK(ui_test_shows("Paused\nTap to continue"));
	int32_t x = lv_obj_get_style_x(h, 0);
	ui_test_wait(1000);
	CHECK_INT(lv_obj_get_style_x(h, 0), x);
	/* Swipes do not move it while paused, a tap goes on */
	swipe(0, 100);
	CHECK(ui_test_shows("Paused\nTap to continue"));
	tap();
	CHECK(!ui_test_shows("Paused\nTap to continue"));
	ui_test_wait(300);
	CHECK(lv_obj_get_style_x(h, 0) > x);
	CHECK(tap_button(LV_SYMBOL_PAUSE));
	CHECK(tap_button(LV_SYMBOL_PLAY));
	CHECK(!ui_test_shows("Paused\nTap to continue"));
}

/* Leaving pauses it, and the game goes with the screen */
TEST(left_while_running)
{
	CHECK(ui_test_open("Snake"));
	tap();
	ui_test_wait(300);
	CHECK(ui_test_back());
	ui_test_wait(3000);
	CHECK(ui_test_open("Snake"));
	CHECK(ui_test_shows("Swipe to start"));
}

/* Steers to the food: along the column first, then the row. The food is placed
 * from the wall clock's second: one on the start row behind the snake waits for
 * another second. */
TEST(eats_and_keeps_the_best)
{
	lv_obj_t *b = NULL;
	int fx = 0, fy = START_Y;
	for (int tries = 0; tries < 5; tries++) {
		CHECK(ui_test_open("Snake"));
		b = board();
		CHECK(b && food(b, &fx, &fy));
		if (fy != START_Y || fx > START_X)
			break;
		CHECK(ui_test_back());
		sleep(1);
	}
	lv_obj_t *h = head(b);
	CHECK(h != NULL);
	if (!h)
		return;

	if (fy == START_Y) {
		tap();
	} else {
		swipe(0, fy < START_Y ? -100 : 100);
		/* Turn on the way into the food's row */
		for (int t = 0; t < 5000 && fx != START_X; t += 16) {
			int32_t y = lv_obj_get_style_y(h, 0) - (CELL - SEG) / 2;
			int cell = fy < START_Y ? y / CELL : (y + CELL - 1) / CELL;
			if (cell == fy) {
				swipe(fx < START_X ? -100 : 100, 0);
				break;
			}
			ui_test_wait(16);
		}
	}
	for (int t = 0; t < 5000 && !ui_test_shows("Score 1"); t += 16)
		ui_test_wait(16);
	CHECK(ui_test_shows("Score 1"));
	ui_test_wait(5000);
	CHECK(ui_test_shows(OVER));
	CHECK(ui_test_shows("Best 1"));

	/* Kept for the next game */
	CHECK(ui_test_back());
	CHECK(ui_test_open("Snake"));
	CHECK(ui_test_shows("Best 1"));
}
