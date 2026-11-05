/* sms_pdu.c: SMS-DELIVER decoded, SMS-SUBMIT encoded. The PDUs are the classic
 * examples of the specification's tutorials and ones made by an independent encoder */

#include <string.h>

#include "test.h"

#include "sms_pdu.h"

static time_t utc(int year, int month, int day, int hour, int min, int sec)
{
	struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
	                 .tm_hour = hour, .tm_min = min, .tm_sec = sec };
	return timegm(&tm);
}

/* With the service centre's address, a national sender, GSM 7-bit */
TEST(deliver)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("07917283010010F5040BC87238880900F10000993092516195800AE8329BFD4697D9EC37", &d));
	CHECK_STR(d.sender, "27838890001");
	CHECK_STR(d.text, "hellohello");
	CHECK_INT(d.time, utc(1999, 3, 29, 13, 16, 59)); /* +02:00 */
	CHECK_INT(d.total, 1);
}

/* A name instead of a number, a time zone west of UTC, lower-case hexadecimal */
TEST(deliver_from_a_name)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00040ed0d637396c7ebbcb00004220923295032902c834", &d));
	CHECK_STR(d.sender, "Vodafone");
	CHECK_STR(d.text, "Hi");
	CHECK_INT(d.time, utc(2024, 3, 1, 2, 59, 30)); /* -03:00 */
}

/* UCS-2, an emoji as a surrogate pair */
TEST(deliver_ucs2)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("07914047449999F9040B910447545512F0000852115001030080160043006500200066006100630069003F0020D83DDE00", &d));
	CHECK_STR(d.sender, "+40744555210");
	CHECK_STR(d.text, "Ce faci? \xf0\x9f\x98\x80");
	CHECK_INT(d.time, utc(2025, 11, 5, 8, 30, 0));
}

/* GSM 7-bit's own characters and its extension table */
TEST(deliver_gsm_extension)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00040B910447545512F0000052115001030080109B720DB4E1BDD71B1F08807EB7CB", &d));
	CHECK_STR(d.text, "\xe2\x82\xac" "5 [ok] @home");
}

/* A part: the concatenation header, then the text after its fill bit */
TEST(deliver_part)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00440B910447545512F00000521150010300800C0500032A0302EE6F399B0C", &d));
	CHECK_STR(d.text, "world");
	CHECK_INT(d.ref, 0x2a);
	CHECK_INT(d.total, 3);
	CHECK_INT(d.seq, 2);
}

TEST(deliver_refused)
{
	struct sms_deliver d;
	CHECK(!sms_deliver_decode("", &d));
	CHECK(!sms_deliver_decode("00", &d));
	CHECK(!sms_deliver_decode("zz040BC87238880900F10000993092516195800AE8329BFD4697D9EC37", &d));
	/* Cut short, the text longer than the PDU */
	CHECK(!sms_deliver_decode("00040BC87238880900F10000993092516195800AE8329BFD", &d));
	/* A status report (MTI 2) */
	CHECK(!sms_deliver_decode("0006D60B911326880736F4111011719551401110117195714000", &d));
	/* 8-bit data (a WAP push) */
	CHECK(!sms_deliver_decode("00040B910447545512F000045211500103008003010203", &d));
}

TEST(submit)
{
	struct sms_submit s;
	CHECK(sms_submit_encode("+46708251358", "hellohello", 0, &s));
	CHECK_INT(s.count, 1);
	CHECK_STR(s.pdu[0], "0011000B916407281553F80000AA0AE8329BFD4697D9EC37");
	CHECK_INT(s.length[0], 23);
}

/* A national number, written with spaces and dashes */
TEST(submit_national)
{
	struct sms_submit s;
	CHECK(sms_submit_encode("0744 555-210", "hellohello", 0, &s));
	CHECK_STR(s.pdu[0], "0011000A8170445525010000AA0AE8329BFD4697D9EC37");
}

TEST(submit_ucs2)
{
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", "Ce faci? \xf0\x9f\x98\x80", 0, &s));
	CHECK_INT(s.count, 1);
	CHECK_STR(s.pdu[0], "0011000B910447545512F00008AA160043006500200066006100630069003F0020D83DDE00");
	CHECK_INT(s.length[0], 36);
}

TEST(submit_gsm_extension)
{
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", "\xe2\x82\xac" "5 [ok] @home", 0, &s));
	CHECK_STR(s.pdu[0], "0011000B910447545512F00000AA109B720DB4E1BDD71B1F08807EB7CB");
}

/* 160 GSM characters in one, 161 in two: 153 and 8, each with its header */
TEST(submit_parts)
{
	char text[201];
	memset(text, 'a', 200);
	text[160] = '\0';
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", text, 7, &s));
	CHECK_INT(s.count, 1);

	text[160] = 'a';
	text[200] = '\0';
	CHECK(sms_submit_encode("+40744555210", text, 7, &s));
	CHECK_INT(s.count, 2);
	CHECK_STR(s.pdu[0], "0051000B910447545512F00000AAA0050003070201C2E170381C0E87C3E170381C0E87C3E170381C0E87"
	                    "C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87"
	                    "C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87"
	                    "C3E170381C0E87C3E170381C0E87C3E170381C0E87C3E170381C0E87C3");
	CHECK_STR(s.pdu[1], "0051000B910447545512F00000AA36050003070202C2E170381C0E87C3E170381C0E87C3E170381C0E87C3"
	                    "E170381C0E87C3E170381C0E87C3E170381C0E03");
	CHECK_INT(s.length[0], (int)strlen(s.pdu[0]) / 2 - 1);
}

/* A part does not end in the middle of an escape (€ is two septets) */
TEST(submit_parts_keep_escapes)
{
	char text[256] = "";
	memset(text, 'a', 152);
	strcpy(text + 152, "\xe2\x82\xac" "bcdefghij");
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", text, 1, &s));
	CHECK_INT(s.count, 2);
	/* 152 septets in the first part (UDL 7 + 152), the escape and € in the second */
	CHECK(strncmp(s.pdu[0] + 28, "9F", 2) == 0);
	CHECK(strncmp(s.pdu[1] + 28, "12", 2) == 0); /* 7 + 2 + 9 */
}

/* UCS-2: 70 units in one, then 67 a part, a surrogate pair kept whole */
TEST(submit_ucs2_parts)
{
	char text[512] = "";
	for (int i = 0; i < 70; i++)
		strcat(text, "\xc4\x83"); /* ă: not in GSM 7-bit */
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", text, 1, &s));
	CHECK_INT(s.count, 1);

	text[0] = '\0';
	for (int i = 0; i < 66; i++)
		strcat(text, "\xc4\x83");
	strcat(text, "\xf0\x9f\x98\x80" "xxx"); /* the pair would be units 67 and 68 */
	CHECK(sms_submit_encode("+40744555210", text, 1, &s));
	CHECK_INT(s.count, 2);
	CHECK(strncmp(s.pdu[0] + 28, "8A", 2) == 0); /* 6 + 66 * 2 */
	CHECK(strncmp(s.pdu[1] + 28, "10", 2) == 0); /* 6 + 5 * 2 */
}

TEST(submit_refused)
{
	struct sms_submit s;
	CHECK(!sms_submit_encode("", "hi", 0, &s));
	CHECK(!sms_submit_encode("Vodafone", "hi", 0, &s));
	CHECK(!sms_submit_encode("+40744555210", "", 0, &s));

	/* 10 parts at most: 10 * 153 GSM characters */
	static char text[2000];
	memset(text, 'a', 1530);
	text[1530] = '\0';
	CHECK(sms_submit_encode("+40744555210", text, 0, &s));
	CHECK_INT(s.count, 10);
	text[1530] = 'a';
	text[1531] = '\0';
	CHECK(!sms_submit_encode("+40744555210", text, 0, &s));
}

/* Broken UTF-8: replaced (U+FFFD, UCS-2), not dropped */
TEST(submit_broken_utf8)
{
	struct sms_submit s;
	CHECK(sms_submit_encode("+40744555210", "a\xff", 0, &s));
	CHECK_STR(s.pdu[0], "0011000B910447545512F00008AA040061FFFD");
}
