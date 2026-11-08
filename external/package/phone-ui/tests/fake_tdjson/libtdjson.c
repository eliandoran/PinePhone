/*
 * libtdjson.so for the tests (see fake_tdjson.h): installed in the Telegram app's lib/
 * as on the phone, it passes TDLib's JSON interface on to the test program's fake, which
 * the program exports (-rdynamic) and resolves when telegram.so loads this library.
 */

#include "../fake_tdjson.h"

/* As td_json_client.h declares them (telegram.so looks them up with dlsym()) */
int td_create_client_id(void);
void td_send(int client_id, const char *request);
const char *td_receive(double timeout);
const char *td_execute(const char *request);

int td_create_client_id(void)
{
	return fake_td_create_client_id();
}

void td_send(int client_id, const char *request)
{
	fake_td_send(client_id, request);
}

const char *td_receive(double timeout)
{
	return fake_td_receive(timeout);
}

const char *td_execute(const char *request)
{
	return fake_td_execute(request);
}
