/*
 * Status checks, as GitHub's GraphQL API gives a commit's (statusCheckRollup): check
 * runs (GitHub Actions, apps) and commit statuses (older CI), and the rollup of all.
 * Shown as GitHub does: a green circle with a check, a red one with a cross, an
 * orange dot while pending, a gray one with a minus when skipped or cancelled.
 */

#include <string.h>

#include "github.h"
#include "ui_symbols.h"

/* NON-NLS-BEGIN: the API's states */
enum check check_state(const struct json *j)
{
	if (!strcmp(json_str(json_get(j, "__typename"), ""), "CheckRun")) {
		if (strcmp(json_str(json_get(j, "status"), ""), "COMPLETED"))
			return CHECK_PENDING; /* queued, in progress, waiting */
		const char *c = json_str(json_get(j, "conclusion"), "");
		if (!strcmp(c, "SUCCESS"))
			return CHECK_PASSED;
		if (!strcmp(c, "SKIPPED") || !strcmp(c, "NEUTRAL") || !strcmp(c, "CANCELLED") || !strcmp(c, "STALE"))
			return CHECK_SKIPPED;
		return CHECK_FAILED; /* failure, timed out, action required, startup failure */
	}
	/* A StatusContext, or the rollup */
	const char *s = json_str(json_get(j, "state"), "");
	if (!strcmp(s, "SUCCESS"))
		return CHECK_PASSED;
	if (!strcmp(s, "PENDING") || !strcmp(s, "EXPECTED"))
		return CHECK_PENDING;
	if (!strcmp(s, "FAILURE") || !strcmp(s, "ERROR"))
		return CHECK_FAILED;
	return CHECK_NONE;
}
/* NON-NLS-END */

void check_icon_set(lv_obj_t *label, enum check state)
{
	/* The colors follow the theme: not constants */
	const char *symbol = "";
	uint32_t color = UI_COLOR_DIM;
	switch (state) {
	case CHECK_FAILED:
		symbol = UI_SYMBOL_CIRCLE_XMARK;
		color = UI_COLOR_RED;
		break;
	case CHECK_PENDING:
		symbol = UI_SYMBOL_CIRCLE;
		color = UI_COLOR_ORANGE;
		break;
	case CHECK_PASSED:
		symbol = UI_SYMBOL_CIRCLE_CHECK;
		color = UI_COLOR_GREEN;
		break;
	case CHECK_SKIPPED:
		symbol = UI_SYMBOL_CIRCLE_MINUS;
		break;
	case CHECK_NONE:
		break;
	}
	lv_label_set_text_static(label, symbol);
	lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
	/* Without one, no room taken in its row */
	lv_obj_set_hidden(label, state == CHECK_NONE);
}
