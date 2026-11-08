/* sms_pdu.c: SMS-DELIVER decoded, SMS-SUBMIT encoded. The PDUs are the classic
 * examples of the specification's tutorials and ones made by an independent encoder */

#include <string.h>

#include "test.h"

#include "modem/sms_pdu.h"

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

/* A concatenation header with a 16-bit reference (IEI 8): 7 octets, no fill bits */
TEST(deliver_part_16bit_reference)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00440B910447545512F00000521150010300800D06080412340302F7B79C4D06", &d));
	CHECK_STR(d.text, "world");
	CHECK_INT(d.ref, 0x1234);
	CHECK_INT(d.total, 3);
	CHECK_INT(d.seq, 2);
}

/* The data coding scheme's other groups: a class (F1), message waiting (C0, and E0 in
 * UCS-2) */
TEST(deliver_coding_groups)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00040B910447545512F000F1521150010300800AE8329BFD4697D9EC37", &d));
	CHECK_STR(d.text, "hellohello");
	CHECK(sms_deliver_decode("00040B910447545512F000C0521150010300800AE8329BFD4697D9EC37", &d));
	CHECK_STR(d.text, "hellohello");
	CHECK(sms_deliver_decode("00040B910447545512F000E0521150010300800400480069", &d));
	CHECK_STR(d.text, "Hi");
	/* 8-bit data with a class (F4), a reserved group (80): not text */
	CHECK(!sms_deliver_decode("00040B910447545512F000F4521150010300800AE8329BFD4697D9EC37", &d));
	CHECK(!sms_deliver_decode("00040B910447545512F000805211500103008004E8329BFD", &d));
}

/* A high surrogate without its low one: U+FFFD, the unit after it kept */
TEST(deliver_lone_surrogate)
{
	struct sms_deliver d;
	CHECK(sms_deliver_decode("00040B910447545512F0000852115001030080060048D83D0069", &d));
	CHECK_STR(d.text, "H\xef\xbf\xbdi");
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
	/* A lead byte without its continuation: replaced, the next character kept */
	CHECK(sms_submit_encode("+40744555210", "\xc4" "b", 0, &s));
	CHECK_STR(s.pdu[0], "0011000B910447545512F00008AA04FFFD0062");
}

/* More septets than 10 parts hold, in escapes (€ is two): refused while encoding */
TEST(submit_too_long_in_escapes)
{
	static char text[767 * 3 + 1];
	for (int i = 0; i < 767; i++)
		memcpy(text + i * 3, "\xe2\x82\xac", 3);
	struct sms_submit s;
	CHECK(!sms_submit_encode("+40744555210", text, 0, &s));
	text[766 * 3] = '\0'; /* 1532 septets: they fit, 11 parts do not */
	CHECK(!sms_submit_encode("+40744555210", text, 0, &s));
	text[760 * 3] = '\0'; /* 1520: 10 parts of 152, an escape not split */
	CHECK(sms_submit_encode("+40744555210", text, 0, &s));
	CHECK_INT(s.count, 10);
}

/* USSD: what the EG25-G gave on Digi's network for *111# in AT+CSCS="UCS2" (dcs 15) */
TEST(ussd_ucs2_menu)
{
	char text[256];
	ussd_decode("0049006400200063006C00690065006E0074003A002000310036003900330030003600370038000A003100200043006F006E00740075006C0020006D00650075000A00320020004F00660065007200740065000A0033002000450078007400720061006F0070007400690075006E0069",
	            15, true, text, sizeof(text));
	CHECK_STR(text, "Id client: 16930678\n1 Contul meu\n2 Oferte\n3 Extraoptiuni");
}

/* In "GSM": the text as it came, its lines (joined with "\r\n") ending in "\n" */
TEST(ussd_gsm_text)
{
	char text[256];
	ussd_decode("Invalid Option.", 15, false, text, sizeof(text));
	CHECK_STR(text, "Invalid Option.");
	ussd_decode("1 Oferta Actuala\r\n2 Upgrade", 15, false, text, sizeof(text));
	CHECK_STR(text, "1 Oferta Actuala\n2 Upgrade");
	/* Hexadecimal-looking text with a GSM dcs stays text */
	ussd_decode("0042", 15, false, text, sizeof(text));
	CHECK_STR(text, "0042");
	/* Bytes that are not UTF-8: not passed on */
	ussd_decode("a\xff", 15, false, text, sizeof(text));
	CHECK_STR(text, "a?");
}

/* A UCS-2 dcs in "GSM": hexadecimal (72: general coding, UCS-2) */
TEST(ussd_ucs2_dcs)
{
	char text[256];
	ussd_decode("0053006F006C0064003A00200035002C0030003000200072006F0074", 72, false, text, sizeof(text));
	CHECK_STR(text, "Sold: 5,00 rot");
	ussd_decode("021900690020", 0x48, false, text, sizeof(text));
	CHECK_STR(text, "\xc8\x99i ");
	/* 17 (0x11): UCS-2 after its language, two GSM characters in the first unit */
	ussd_decode("656E00480069", 0x11, false, text, sizeof(text));
	CHECK_STR(text, "Hi");
	/* Not hexadecimal after all: text */
	ussd_decode("Hi there", 72, false, text, sizeof(text));
	CHECK_STR(text, "Hi there");
	/* Surrogates: an emoji */
	ussd_decode("D83DDE00", 15, true, text, sizeof(text));
	CHECK_STR(text, "\xf0\x9f\x98\x80");
	/* Too long for out: cut, still a string */
	char small[4];
	ussd_decode("0041004200430044", 15, true, small, sizeof(small));
	CHECK_STR(small, "ABC");
}

TEST(ussd_encode)
{
	char hex[64];
	CHECK(ussd_encode("*111#", hex, sizeof(hex)));
	CHECK_STR(hex, "002A0031003100310023");
	CHECK(ussd_encode("\xc8\x99", hex, sizeof(hex)));
	CHECK_STR(hex, "0219");
	CHECK(ussd_encode("", hex, sizeof(hex)));
	CHECK_STR(hex, "");
	CHECK(ussd_encode("\xf0\x9f\x98\x80", hex, sizeof(hex))); /* a surrogate pair */
	CHECK_STR(hex, "D83DDE00");
	CHECK(!ussd_encode("*111#", hex, 20)); /* 20 digits and the end */
}
