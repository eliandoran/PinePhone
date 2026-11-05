/*
 * What phone-ui looks for in github.so (see external.c): the headers it was
 * built against, and the function that opens the app.
 */

#include "abi.h"
#include "github.h"

const char app_abi[] = UI_ABI;

void app_open(void)
{
	github_open();
}
