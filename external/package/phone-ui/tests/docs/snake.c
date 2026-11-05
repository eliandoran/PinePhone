/* docs/apps/snake.md's screenshots (included by test_docs.c) */

#define SNAKE_CELL 40 /* snake.c's */
#define SNAKE_FOOD 30
#define SNAKE_SEG 32
#define SNAKE_COLS 17
#define SNAKE_ROWS 28

/* The finger without ui_test_settle(), which waits for animations to end: the
 * snake's steps are one after the other */
static void snake_finger(int32_t x, int32_t y, int32_t dx, int32_t dy)
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

/* The board: what the message is in */
static lv_obj_t *snake_board(void)
{
	lv_obj_t *m = ui_test_find("Swipe to start");
	return m ? lv_obj_get_parent(m) : NULL;
}

/* The food's cell */
static bool snake_food(lv_obj_t *b, int *x, int *y)
{
	for (uint32_t i = 0; i < lv_obj_get_child_count(b); i++) {
		lv_obj_t *o = lv_obj_get_child(b, (int32_t)i);
		if (lv_obj_get_style_width(o, 0) == SNAKE_FOOD) {
			*x = (lv_obj_get_style_x(o, 0) - (SNAKE_CELL - SNAKE_FOOD) / 2) / SNAKE_CELL;
			*y = (lv_obj_get_style_y(o, 0) - (SNAKE_CELL - SNAKE_FOOD) / 2) / SNAKE_CELL;
			return true;
		}
	}
	return false;
}

/* The head, by its color */
static lv_obj_t *snake_head(lv_obj_t *b)
{
	for (uint32_t i = 0; b && i < lv_obj_get_child_count(b); i++) {
		lv_obj_t *o = lv_obj_get_child(b, (int32_t)i);
		if (lv_color_eq(lv_obj_get_style_bg_color(o, 0), lv_color_hex(0x1f9e40)))
			return o;
	}
	return NULL;
}

enum { SNAKE_RIGHT, SNAKE_DOWN, SNAKE_LEFT, SNAKE_UP };

static void snake_swipe(int dir)
{
	static const int dx[4] = { 100, 0, -100, 0 }, dy[4] = { 0, 100, 0, -100 };
	snake_finger(360, 900, dx[dir], dy[dir]);
}

/* Plays as a player who heads for the food, until `score` pieces are eaten (false if
 * the game ends first) */
static bool snake_play_to(lv_obj_t *b, lv_obj_t *h, int score)
{
	char want[16];
	snprintf(want, sizeof(want), "Score %d", score);
	int32_t px = lv_obj_get_style_x(h, 0), py = lv_obj_get_style_y(h, 0);
	int swiped = -1, swiped_at_x = -1, swiped_at_y = -1;

	snake_finger(360, 900, 0, 0); /* a tap starts it, heading right */
	for (int t = 0; t < 60000 && !ui_test_shows(want); t += 16) {
		if (ui_test_shows("Game over\nTap to play again"))
			return false;
		ui_test_wait(16);
		int32_t x = lv_obj_get_style_x(h, 0) - (SNAKE_CELL - SNAKE_SEG) / 2;
		int32_t y = lv_obj_get_style_y(h, 0) - (SNAKE_CELL - SNAKE_SEG) / 2;
		int32_t mx = lv_obj_get_style_x(h, 0) - px, my = lv_obj_get_style_y(h, 0) - py;
		px += mx;
		py += my;
		if ((!mx && !my) || (x % SNAKE_CELL == 0 && y % SNAKE_CELL == 0))
			continue; /* between steps: which cell it heads to is not clear */
		int moving = mx > 0 ? SNAKE_RIGHT : mx < 0 ? SNAKE_LEFT : my > 0 ? SNAKE_DOWN : SNAKE_UP;
		/* The cell it is on its way to, where a turn swiped now happens */
		int cx = moving == SNAKE_RIGHT ? (x + SNAKE_CELL - 1) / SNAKE_CELL : x / SNAKE_CELL;
		int cy = moving == SNAKE_DOWN ? (y + SNAKE_CELL - 1) / SNAKE_CELL : y / SNAKE_CELL;
		int fx, fy;
		if (!snake_food(b, &fx, &fy))
			return false;

		int horizontal = fx > cx ? SNAKE_RIGHT : fx < cx ? SNAKE_LEFT : -1;
		int vertical = fy > cy ? SNAKE_DOWN : fy < cy ? SNAKE_UP : -1;
		int dir = moving;
		if (horizontal >= 0 && horizontal != (moving + 2) % 4)
			dir = horizontal;
		else if (vertical >= 0 && vertical != (moving + 2) % 4)
			dir = vertical;
		else if (horizontal >= 0 || vertical >= 0) /* the food behind: around */
			dir = moving % 2 ? (cx > SNAKE_COLS / 2 ? SNAKE_LEFT : SNAKE_RIGHT) :
			                   (cy > SNAKE_ROWS / 2 ? SNAKE_UP : SNAKE_DOWN);
		if (dir != moving && (dir != swiped || cx != swiped_at_x || cy != swiped_at_y)) {
			snake_swipe(dir);
			swiped = dir;
			swiped_at_x = cx;
			swiped_at_y = cy;
		}
	}
	return ui_test_shows(want);
}

/* snake.md: a game paused, the best score of earlier games beside the score */
TEST(snake_paused)
{
	ui_setting_set_int("snake_best", 23);
	CHECK(ui_test_open("Snake"));
	CHECK(ui_test_shows("Swipe to start"));
	CHECK(ui_test_shows("Score 0"));
	CHECK(ui_test_shows("Best 23"));
	lv_obj_t *b = snake_board(), *h = snake_head(b);
	CHECK(b && h);
	if (!b || !h)
		return;
	CHECK(snake_play_to(b, h, 6));
	/* The header's button */
	lv_obj_t *pause = ui_test_find(LV_SYMBOL_PAUSE);
	CHECK(pause != NULL);
	if (!pause)
		return;
	lv_area_t a;
	lv_obj_get_coords(pause, &a);
	snake_finger((a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, 0, 0);
	CHECK(ui_test_shows("Paused\nTap to continue"));
	CHECK(ui_test_shows(LV_SYMBOL_PLAY));
	CHECK(ui_test_shows("Score 6"));
	CHECK(ui_test_shows("Best 23"));
	shot("snake-paused");
}
