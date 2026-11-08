/*
 * What phone-ui looks for in assistant.so (see external.c): the headers it was
 * built against, the function that opens the app, and its name translated.
 */

#include "abi.h"
#include "assistant.h"

const char app_abi[] = UI_ABI;

void app_open(void)
{
	assistant_open();
}

const char *app_name(void)
{
	return _("Assistant");
}
