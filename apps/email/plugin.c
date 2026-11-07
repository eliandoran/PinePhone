/*
 * What phone-ui looks for in email.so (see external.c): the headers it was built
 * against, and the function that opens the app.
 */

#include "abi.h"
#include "email.h"

const char app_abi[] = UI_ABI;

void app_open(void)
{
	email_open();
}
