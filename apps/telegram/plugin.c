/*
 * What phone-ui looks for in telegram.so (see external.c): the headers it was
 * built against, and the function that opens the app.
 */

#include "abi.h"
#include "telegram.h"

const char app_abi[] = UI_ABI;

void app_open(void)
{
	telegram_open();
}
