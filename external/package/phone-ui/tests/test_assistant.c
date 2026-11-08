/*
 * Assistant (assistant/) with DeepSeek's API served by the tests (fake_curl.c):
 * streamed answers, errors, web searches, the chats kept, the settings.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "test.h"
#include "ui_test.h"

#define SETTINGS "/var/lib/phone-ui/settings"
#define DATA "/var/lib/assistant"
#define CHAT "https://api.deepseek.com/chat/completions"
#define SEARCH "https://api.deepseek.com/v1/tools/search"
#define KEY "assistant.deepseek.key=sk-test\n"
#define SSE "text/event-stream"
/* A streamed chunk of the answer */
#define DELTA(json) "data: {\"choices\":[{\"index\":0,\"delta\":" json "}]}\n\n"
#define DONE "data: [DONE]\n\n"

static char *file(const char *path)
{
	static char buf[16384];
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	if (f)
		fclose(f);
	buf[n] = '\0';
	return buf;
}

/* The one chat kept, "" if none */
static char *chat_file(void)
{
	static char path[300];
	path[0] = '\0';
	DIR *d = opendir(DATA);
	struct dirent *ent;
	while (d && (ent = readdir(d)))
		if (strstr(ent->d_name, ".json"))
			snprintf(path, sizeof(path), DATA "/%s", ent->d_name);
	if (d)
		closedir(d);
	return path[0] ? file(path) : path;
}

static void net_wait(void)
{
	ui_test_wait(300);
	ui_test_settle();
}

TEST_SETUP
{
	ui_test_reset();
}

/* A new chat, the question asked */
static bool ask(const char *question)
{
	if (!ui_test_open("Assistant") || !ui_test_tap_text(LV_SYMBOL_EDIT))
		return false;
	bool ok = ui_test_type(question) && ui_test_type("\n");
	net_wait();
	return ok;
}

TEST(no_chats)
{
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_shows("No chats"));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows("Ask a question below"));
}

TEST(no_api_key)
{
	CHECK(ask("Hello"));
	CHECK(ui_test_shows("Hello"));
	CHECK(ui_test_shows("No DeepSeek API key: add one in the settings (" LV_SYMBOL_SETTINGS
	                    "), then tap here."));
	CHECK_INT(ui_test_request_count(), 0);
}

TEST(answer_streamed)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE,
	              ": keep-alive\n\n"
	              DELTA("{\"role\":\"assistant\",\"content\":\"\"}")
	              DELTA("{\"content\":\"Hello\"}")
	              DELTA("{\"content\":\", I am here.\"}")
	              DONE);
	ui_test_serve_in_pieces(10); /* a piece every 50 ms */
	CHECK(ask("Are you there?"));
	/* Arriving */
	CHECK(ui_test_wait_for("Hello", 2000));
	CHECK(ui_test_shows("Writing..."));
	CHECK(ui_test_wait_for("Hello, I am here.", 2000));
	ui_test_wait(300); /* the end of the stream */
	CHECK(ui_test_shows("Hello, I am here."));
	CHECK(!ui_test_shows("Writing..."));
	char now[8];
	time_t t = time(NULL);
	strftime(now, sizeof(now), "%H:%M", localtime(&t));
	lv_obj_t *found[4];
	CHECK(ui_test_find_all(now, found, 4) >= 2); /* the question's and the answer's */

	const struct ui_test_request *q = ui_test_requested(CHAT);
	CHECK(q && q->body);
	if (!q || !q->body)
		return;
	CHECK(strstr(q->body, "\"model\":\"deepseek-flash\",\"stream\":true,") != NULL);
	CHECK(strstr(q->body, "\"thinking\":{\"type\":\"disabled\"}") != NULL);
	CHECK(strstr(q->body, "{\"role\":\"user\",\"content\":\"Are you there?\"}") != NULL);
	CHECK(strstr(q->body, "\"tools\"") == NULL);
	CHECK(strstr(q->headers, "Authorization: Bearer sk-test\n") != NULL);
	CHECK(strstr(q->headers, "Content-Type: application/json\n") != NULL);
	CHECK(strstr(chat_file(), "Hello, I am here.") != NULL);

	/* In the list, and opened again */
	CHECK(ui_test_back());
	CHECK(ui_test_shows_part("Hello, I am here."));
	CHECK(ui_test_tap_text("Are you there?"));
	CHECK(ui_test_shows("Hello, I am here."));
}

/* Its earlier messages go with the next question */
TEST(follow_up)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Second answer\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE, DELTA("{\"content\":\"First answer\"}") DONE);
	CHECK(ask("One"));
	CHECK(ui_test_shows("First answer"));
	CHECK(ui_test_tap_field("Ask anything"));
	CHECK(ui_test_type("Two\n"));
	net_wait();
	CHECK(ui_test_shows("Second answer"));
	const struct ui_test_request *q = ui_test_request(-1);
	CHECK(q && strstr(q->body, "{\"role\":\"user\",\"content\":\"One\"},"
	                  "{\"role\":\"assistant\",\"content\":\"First answer\"},"
	                  "{\"role\":\"user\",\"content\":\"Two\"}]}") != NULL);
}

TEST(thinking)
{
	ui_test_write(SETTINGS, KEY "assistant.thinking=1\nassistant.model=deepseek-v4-pro\n");
	ui_test_serve(CHAT, 200, SSE,
	              DELTA("{\"reasoning_content\":\"Let me see.\"}")
	              DELTA("{\"content\":\"\\n\\nFour.\"}")
	              DONE);
	ui_test_serve_in_pieces(10);
	CHECK(ask("2 + 2?"));
	CHECK(ui_test_wait_for("Thinking...", 2000));
	ui_test_wait(1000);
	CHECK(ui_test_shows("Four.")); /* the new lines before it dropped */
	const struct ui_test_request *q = ui_test_requested(CHAT);
	CHECK(q && strstr(q->body, "\"model\":\"deepseek-v4-pro\"") != NULL);
	CHECK(q && strstr(q->body, "\"thinking\":{\"type\":\"enabled\"}") != NULL);
}

TEST(offline_then_retried)
{
	ui_test_write(SETTINGS, KEY);
	CHECK(ask("Hello"));
	const char *offline = LV_SYMBOL_WARNING " No internet connection. Tap to try again.";
	CHECK(ui_test_shows(offline));
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Back online\"}") DONE);
	CHECK(ui_test_tap_text(offline));
	net_wait();
	CHECK(ui_test_shows("Back online"));
	CHECK(!ui_test_shows(offline));
}

TEST(api_errors)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 401, "application/json",
	              "{\"error\":{\"message\":\"Authentication Fails\",\"type\":\"authentication_error\"}}");
	CHECK(ask("Hello"));
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " Authentication Fails (401). Tap to try again."));

	ui_test_serve(CHAT, 503, "text/html", "<h1>Service Unavailable</h1>");
	CHECK(ui_test_tap_text(LV_SYMBOL_WARNING " Authentication Fails (401). Tap to try again."));
	net_wait();
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " HTTP error 503. Tap to try again."));

	ui_test_serve(CHAT, 200, SSE, DONE);
	CHECK(ui_test_tap_text(LV_SYMBOL_WARNING " HTTP error 503. Tap to try again."));
	net_wait();
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " Empty answer. Tap to try again."));
}

/* The model calls web_search: the app searches, then asks it again with the results */
TEST(web_search)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"It runs Linux.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE,
	                   DELTA("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"type\":\"function\","
	                         "\"function\":{\"name\":\"web_search\",\"arguments\":\"\"}}]}")
	                   DELTA("{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"query\\\":\"}}]}")
	                   DELTA("{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"\\\"pinephone\\\"}\"}}]}")
	                   DONE);
	ui_test_serve(SEARCH, 200, "application/json",
	              "{\"results\":[{\"title\":\"PinePhone\",\"url\":\"https://pine64.org/devices/pinephone\","
	              "\"summary\":\"A Linux phone.\",\"date\":\"2024-01-01T00:00:00Z\"}]}");
	CHECK(ask("What is a PinePhone?"));
	ui_test_wait(1000);
	CHECK(ui_test_shows(LV_SYMBOL_OK " Searched the web: pinephone"));
	CHECK(ui_test_shows("It runs Linux."));

	const struct ui_test_request *s = ui_test_requested(SEARCH);
	CHECK(s && s->body && !strcmp(s->body, "{\"query\":\"pinephone\",\"count\":5}"));
	CHECK(s && strstr(s->headers, "Authorization: Bearer sk-test\n") != NULL);
	const struct ui_test_request *first = ui_test_request(0), *last = ui_test_request(-1);
	CHECK(first && strstr(first->body, "\"tool_choice\":\"auto\"") != NULL);
	CHECK(last && last != first && !strcmp(last->url, CHAT));
	CHECK(last && strstr(last->body, "\"tool_calls\":[{\"id\":\"call_1\"") != NULL);
	CHECK(last && strstr(last->body, "{\"role\":\"tool\",\"tool_call_id\":\"call_1\","
	                     "\"content\":\"1. PinePhone (2024-01-01)\\u000ahttps://pine64.org/devices/pinephone\\u000a"
	                     "A Linux phone.\\u000a\\u000a\"}") != NULL);

	/* Kept with its steps, shown again */
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("What is a PinePhone?"));
	CHECK(ui_test_shows(LV_SYMBOL_OK " Searched the web: pinephone"));
	CHECK(ui_test_shows("It runs Linux."));
}

TEST(web_search_offline)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"I could not look it up.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE,
	                   DELTA("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":"
	                         "{\"arguments\":\"{\\\"query\\\":\\\"news\\\"}\"}}]}")
	                   DONE);
	CHECK(ask("News?"));
	ui_test_wait(1000);
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " Web search failed: news"));
	CHECK(ui_test_shows("I could not look it up."));
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "The search failed: no internet connection.") != NULL);
}

/* Left while it answers: what came is kept */
TEST(left_while_answering)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"A long answer\"}")
	              DELTA("{\"content\":\" that goes on\"}") DONE);
	ui_test_serve_in_pieces(10);
	CHECK(ask("Tell me"));
	CHECK(ui_test_wait_for("A long answer", 2000));
	CHECK(ui_test_back());
	ui_test_wait(1000);
	CHECK(strstr(chat_file(), "A long answer") != NULL);
	CHECK(strstr(chat_file(), "that goes on") == NULL);
}

TEST(not_answered_when_opened_again)
{
	ui_test_write(SETTINGS, KEY);
	CHECK(ask("Hello"));
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Hello"));
	CHECK(ui_test_shows("Not answered. Tap to try again."));
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Now answered\"}") DONE);
	CHECK(ui_test_tap_text("Not answered. Tap to try again."));
	net_wait();
	CHECK(ui_test_shows("Now answered"));
}

TEST(chat_deleted)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Hi\"}") DONE);
	CHECK(ask("Hello"));
	CHECK(chat_file()[0]);
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH "  Delete chat"));
	CHECK(!chat_file()[0]);
	CHECK(ui_test_shows("No chats"));
}

TEST(settings)
{
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_tap_text(LV_SYMBOL_SETTINGS));
	CHECK(ui_test_tap_field("API key"));
	CHECK(ui_test_type("sk-typed"));
	CHECK(ui_test_tap_text("deepseek-v4-pro"));
	lv_obj_t *switches[2];
	CHECK_INT(ui_test_find_widgets(&lv_switch_class, switches, 2), 2);
	ui_test_tap(switches[1]); /* web search */
	CHECK(ui_test_back());
	const char *s = file(SETTINGS);
	CHECK(strstr(s, "assistant.deepseek.key=sk-typed\n") != NULL);
	CHECK(strstr(s, "assistant.model=deepseek-v4-pro\n") != NULL);
	CHECK(strstr(s, "assistant.search=1\n") != NULL);
}

/* In Romanian: the list, a chat with a search, its errors, the settings */
TEST(romanian)
{
	ui_test_write(SETTINGS, "language=ro\n");
	CHECK(ui_test_open("Asistent"));
	CHECK(ui_test_shows("Nicio conversație"));
	CHECK(ui_test_tap_text(LV_SYMBOL_EDIT));
	CHECK(ui_test_shows("Puneți o întrebare mai jos"));
	CHECK(ui_test_tap_field("Întreabă orice"));
	CHECK(ui_test_type("Salut\n"));
	net_wait();
	CHECK(ui_test_shows("Nicio cheie API DeepSeek: adăugați-o în setări (" LV_SYMBOL_SETTINGS
	                    "), apoi atingeți aici."));

	CHECK(ui_test_tap_text(LV_SYMBOL_SETTINGS));
	CHECK(ui_test_shows("Setări Asistent"));
	CHECK(ui_test_shows("Rapid, ieftin"));
	CHECK(ui_test_shows("Căutare pe web"));
	CHECK(ui_test_tap_field("Cheie API"));
	CHECK(ui_test_type("sk-test"));
	lv_obj_t *switches[2];
	CHECK_INT(ui_test_find_widgets(&lv_switch_class, switches, 2), 2);
	ui_test_tap(switches[1]); /* web search */
	CHECK(ui_test_back());

	const char *offline = LV_SYMBOL_WARNING " Fără conexiune la internet. Atingeți pentru a reîncerca.";
	CHECK(ui_test_tap_text("Nicio cheie API DeepSeek: adăugați-o în setări (" LV_SYMBOL_SETTINGS
	                       "), apoi atingeți aici."));
	net_wait();
	CHECK(ui_test_shows(offline));
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Bună!\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE,
	                   DELTA("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":"
	                         "{\"arguments\":\"{\\\"query\\\":\\\"vremea\\\"}\"}}]}")
	                   DONE);
	CHECK(ui_test_tap_text(offline));
	ui_test_wait(1000);
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " Căutarea pe web a eșuat: vremea"));
	CHECK(ui_test_shows("Bună!"));

	CHECK(ui_test_back());
	CHECK(ui_test_shows_part("Bună!"));
}

/* A new chat from the round button at the bottom right, in reach of the thumb */
TEST(new_chat_button)
{
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_action_button(LV_SYMBOL_EDIT));
}

/* Scrolled to the end, the oldest chat is not under the button */
TEST(new_chat_button_clear_of_the_last)
{
	time_t now = time(NULL);
	for (int i = 0; i < 12; i++) {
		char path[64], text[160];
		long long t = (long long)now - 3600 * (i + 1);
		snprintf(path, sizeof(path), DATA "/%lld.json", t);
		snprintf(text, sizeof(text),
		         "{\"messages\": [{\"role\": \"user\", \"time\": %lld, \"content\": \"Chat %d\"}]}", t, i);
		ui_test_write(path, text);
	}
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_clear_of_action_button("Chat 11", LV_SYMBOL_EDIT));
}

/* Pasted with spaces around: kept without; Enter hides the keyboard and keeps it */
TEST(api_key_trimmed)
{
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_tap_text(LV_SYMBOL_SETTINGS));
	CHECK(ui_test_tap_field("API key"));
	CHECK(ui_test_type("  sk-typed  \n"));
	CHECK(!ui_test_textarea());
	CHECK(strstr(file(SETTINGS), "assistant.deepseek.key=sk-typed\n") != NULL);
}

/* One question at a time: typed while it answers, it waits in the field */
TEST(asked_while_answering)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Here it is\"}") DONE);
	ui_test_network_hold(true);
	CHECK(ask("Tell me"));
	CHECK(ui_test_shows("Waiting..."));
	CHECK(ui_test_tap_field("Ask anything"));
	lv_obj_t *field = ui_test_textarea();
	CHECK(ui_test_type("And then?\n"));
	CHECK(field && !strcmp(lv_textarea_get_text(field), "And then?"));
	ui_test_network_hold(false);
	CHECK(ui_test_wait_for("Here it is", 2000));
	CHECK_INT(ui_test_request_count(), 1);
}

/* Nowhere to keep chats: answered all the same */
TEST(chat_not_kept)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_write(DATA, "not a folder");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Hi\"}") DONE);
	CHECK(ask("Hello"));
	CHECK(ui_test_shows("Hi"));
}

/* A step without what it sent the model (an earlier version's): left out */
TEST(old_step_left_out)
{
	ui_test_write(DATA "/1773481260.json",
	              "{\"messages\": [{\"role\": \"user\", \"time\": 1773481260, \"content\": \"News?\"},"
	              "{\"role\": \"step\", \"time\": 1773481261, \"content\": \"Searched the web: news\"},"
	              "{\"role\": \"assistant\", \"time\": 1773481262, \"content\": \"Nothing new.\"}]}");
	CHECK(ui_test_open("Assistant"));
	CHECK(ui_test_tap_text("News?"));
	CHECK(ui_test_shows("Nothing new."));
	CHECK(!ui_test_shows_part("Searched the web"));
}

/* The chat's delete, thought better of */
TEST(chat_delete_cancelled)
{
	ui_test_write(SETTINGS, KEY);
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Hi\"}") DONE);
	CHECK(ask("Hello"));
	CHECK(ui_test_tap_text(LV_SYMBOL_TRASH));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows(LV_SYMBOL_TRASH "  Delete chat"));
	CHECK(ui_test_shows("Hi"));
	CHECK(chat_file()[0]);
	/* Back to the list, then out of the app */
	CHECK(ui_test_back());
	CHECK(ui_test_shows("Hello"));
	CHECK(ui_test_back());
	CHECK(ui_at_home());
}

#define SEARCH_CALL(arguments) \
	DELTA("{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\",\"function\":{\"arguments\":\"" arguments "\"}}]}")

/* The search refused: the model is told why; a call past the ones kept is not made */
TEST(web_search_server_error)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"The search is down.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE,
	                   SEARCH_CALL("{\\\"query\\\":\\\"news\\\"}")
	                   DELTA("{\"tool_calls\":[{\"index\":99,\"id\":\"call_99\",\"function\":"
	                         "{\"arguments\":\"{\\\"query\\\":\\\"more\\\"}\"}}]}")
	                   DONE);
	ui_test_serve(SEARCH, 500, "text/plain", "Internal Server Error");
	CHECK(ask("News?"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("The search is down."));
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "The search failed (HTTP 500).") != NULL);
	CHECK(last && strstr(last->body, "call_99") == NULL);
	CHECK(!ui_test_shows(LV_SYMBOL_OK " Searched the web: more"));
}

TEST(web_search_no_results)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Nothing found.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE, SEARCH_CALL("{\\\"query\\\":\\\"zzxq\\\"}") DONE);
	ui_test_serve(SEARCH, 200, "application/json", "{\"results\":[]}");
	CHECK(ask("zzxq?"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("Nothing found."));
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "No results.") != NULL);
}

/* Asked to search for nothing: not searched, the model told */
TEST(web_search_without_a_query)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Sorry.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE, SEARCH_CALL("{}") DONE);
	CHECK(ask("Search"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("Sorry."));
	CHECK(ui_test_requested(SEARCH) == NULL);
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "The search failed (HTTP 400).") != NULL);
}

/* Thinking, then a search: the thoughts go back with the call, as DeepSeek asks */
TEST(thinking_then_web_search)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\nassistant.thinking=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"It runs Linux.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE,
	                   DELTA("{\"reasoning_content\":\"Better look it up.\"}")
	                   SEARCH_CALL("{\\\"query\\\":\\\"pinephone\\\"}") DONE);
	ui_test_serve(SEARCH, 200, "application/json",
	              "{\"results\":[{\"title\":\"PinePhone\",\"url\":\"https://pine64.org\",\"summary\":\"A phone.\"}]}");
	CHECK(ask("What is a PinePhone?"));
	ui_test_wait(1000);
	CHECK(ui_test_shows("It runs Linux."));
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "\"reasoning_content\":\"Better look it up.\",\"tool_calls\"") != NULL);
}

/* Not answered, asked again: both questions go as one message */
TEST(asked_again_after_no_answer)
{
	ui_test_write(SETTINGS, KEY);
	CHECK(ask("Hello"));
	CHECK(ui_test_shows(LV_SYMBOL_WARNING " No internet connection. Tap to try again."));
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"Hi there\"}") DONE);
	CHECK(ui_test_tap_field("Ask anything"));
	CHECK(ui_test_type("Anyone?\n"));
	net_wait();
	CHECK(ui_test_shows("Hi there"));
	const struct ui_test_request *last = ui_test_request(-1);
	CHECK(last && strstr(last->body, "\"content\":\"Hello\\n\\nAnyone?\"") != NULL);
}

/* Left while it searches: the call and its search dropped, the question kept */
TEST(left_while_searching)
{
	ui_test_write(SETTINGS, KEY "assistant.search=1\n");
	ui_test_serve(CHAT, 200, SSE, DELTA("{\"content\":\"It runs Linux.\"}") DONE);
	ui_test_serve_once(CHAT, 200, SSE, SEARCH_CALL("{\\\"query\\\":\\\"pinephone\\\"}") DONE);
	static char results[4096];
	char *p = results + sprintf(results, "{\"results\":[");
	for (int i = 0; i < 20; i++)
		p += sprintf(p, "%s{\"title\":\"Result %d\",\"url\":\"https://example.com/%d\"}", i ? "," : "", i, i);
	strcpy(p, "]}");
	ui_test_serve(SEARCH, 200, "application/json", results);
	ui_test_serve_in_pieces(16);
	CHECK(ask("What is a PinePhone?"));
	CHECK(ui_test_wait_for("Searching the web: pinephone", 3000));
	CHECK(ui_test_back());
	ui_test_wait(2000);
	CHECK(strstr(chat_file(), "What is a PinePhone?") != NULL);
	CHECK(strstr(chat_file(), "call_1") == NULL);
	CHECK(strstr(chat_file(), "It runs Linux.") == NULL);
}
