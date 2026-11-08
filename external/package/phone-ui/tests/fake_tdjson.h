/*
 * TDLib for the Telegram plugin app (apps/telegram, test_telegram.c): fake_tdjson.c,
 * linked into the test program, answers what telegram.so asks through libtdjson's JSON
 * interface. telegram.so loads fake_tdjson/libtdjson.c's library from its lib/ as on the
 * phone, which hands each call to the program (fake_td_*(), exported with -rdynamic).
 *
 * The client goes through TDLib's authorization states: signed out it asks for a phone
 * number, then the code (FAKE_TD_CODE); with an account (ui_test_td_account()) it is ready
 * at once. Answers and updates are queued as requests come and handed out by
 * td_receive(), in that order. Requests the fake does not know are answered "ok" and logged.
 */

#ifndef PHONE_UI_FAKE_TDJSON_H
#define PHONE_UI_FAKE_TDJSON_H

#include <stdbool.h>

#define FAKE_TD_CODE "12345"
#define FAKE_TD_ME 777000111 /* the account's user id */

/* Signed in with this account (getMe); last name, username, phone number may be "" */
void ui_test_td_account(const char *first, const char *last, const char *username, const char *phone);
/* Signing in asks for this password after the code (two-step verification), with that hint */
void ui_test_td_password(const char *password, const char *hint);
void ui_test_td_bio(const char *bio); /* getUserFullInfo's */
void ui_test_td_birthdate(int day, int month, int year); /* its too; year 0: unknown */
/* The account's profile photo, a JPEG at `path` (both sizes), downloaded when asked */
void ui_test_td_photo(const char *path);
/* A contact (getContacts, updateUser before it), its status as TDLib's JSON (NULL:
 * userStatusRecently), with a profile photo at `photo` (a JPEG, downloaded when asked; NULL
 * none). createPrivateChat with it makes a chat of its name (updateNewChat), with no
 * messages. In the order added: getContacts gives them unsorted. */
void ui_test_td_contact(long long id, const char *first, const char *last, const char *status, const char *photo);
/* A user's new status (updateUserStatus), JSON */
void ui_test_td_status(long long id, const char *status);
/* A username setUsername refuses as USERNAME_OCCUPIED */
void ui_test_td_username_taken(const char *username);
/* The next request of that @type is answered with this error */
void ui_test_td_fail(const char *type, int code, const char *message);
/* While held, requests are not answered: their answers wait until it is released */
void ui_test_td_hold(bool hold);
/* A message (TDLib's JSON) in its chat's history: getChatHistory gives it, getRepliedMessage
 * the one it answers. Text sent (sendMessage) shows up as TDLib does: pending, then sent
 * with another id, its reply_to as asked, and in the history. */
void ui_test_td_message(const char *json);
/* The instant view of a page (getWebPageInstantView of that URL): its webPageInstantView
 * JSON; other pages have none (404) */
void ui_test_td_instant_view(const char *url, const char *json);
/* A file downloadFile gives, a JPEG at `path` */
void ui_test_td_file(int id, const char *path);
/* The network gone (updateConnectionState: waiting for it), text sent stays pending; back
 * (ready), what was sent meanwhile goes */
void ui_test_td_offline(bool offline);
/* The next text sent is refused by the server: failed (updateMessageSendFailed) with
 * this error, which TDLib may retry (resendMessages) or not. Failed messages stay in the
 * chat until resent or deleted (deleteMessages). */
void ui_test_td_send_refused(int code, const char *message, bool can_retry);
/* An update (JSON) handed out as if TDLib had sent it */
void ui_test_td_update(const char *json);
/* The last request of that @type as sent (JSON), NULL if none; how many */
const char *ui_test_td_requested(const char *type);
unsigned ui_test_td_request_count(const char *type);

#endif
