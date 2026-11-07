/*
 * What phone-ui looks for in telegram.so (see external.c): the headers it was
 * built against, the function that opens the app, and the one that starts TDLib
 * with phone-ui once signed in (its notifications).
 */

#include "abi.h"
#include "telegram.h"

const char app_abi[] = UI_ABI;

void app_open(void)
{
	telegram_open();
}

void app_start(void)
{
	telegram_start_signed_in();
}
