/* Computers asking for SSH access (pairing.c): their key sent over the USB network,
 * allowed or denied on the phone. Here the network is the sandbox's loopback
 * (PAIRING_DEVICE "lo" for pairing.o). */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define KEYS "/var/lib/ssh/authorized_keys"
#define PORT 7022

#define ED25519 "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFDfx3gDrM25JFBgrXqcFOLxTCIe3PTMiMSU2kr6SQQM"
#define ED25519_FP "SHA256:6dNTP3YrWGO2iwLrslAGHu8ZhaN6dHf8uC1/bZvtRfk"
#define RSA "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQCnMuG0D22oh/Uk/kmm6xvsoN4U3oEZ7FdCw9xyxM5ox7Ymlc/CQPE4" \
	"/q4EsjafCgXTLGmSpvtP9IwBR0ztJu2W/RtDeL6dqxj2aqzG8I/b/70HrqUdDx/5MoArL21nO2QFtyb8UnUnc0ecwtyEjByG" \
	"Ocr+gVJ5ySX7Pi1AJQL7N/1rdzF5ppWFto7sWeRWY8PMvMMfmmuqGSe5iCK+qdAkNVVSfbUfM1R5faBVoc6DnZdevFM8/FU0" \
	"EQlQk4rEYsIOl5/X1+mX0TwKwzVAkQXFUkdlEbuo7AzGFFxgbo6KoJvymsLwMsa6leL3F4Kyl2UvHz8regAFvgRhu+mv1B0n"
#define RSA_FP "SHA256:84FQ5M3dwElElX5+3ruK7Jw47cY8hLyQRKm1cKM7iUY"

TEST_SETUP
{
	ui_test_start();
}

/* A computer: connected to the phone's pairing port, `line` sent (with a newline) */
static int computer(const char *line)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		printf("cannot connect: %s\n", strerror(errno));
		if (fd >= 0)
			close(fd);
		return -1;
	}
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s\n", line);
	if (write(fd, buf, strlen(buf)) < 0)
		printf("cannot send: %s\n", strerror(errno));
	fcntl(fd, F_SETFL, O_NONBLOCK);
	return fd;
}

/* The phone's answer, read while the UI runs: "" if none came within ms */
static const char *answer(int fd, uint32_t ms)
{
	static char buf[64];
	size_t len = 0;
	for (uint32_t t = 0; t <= ms; t += 16) {
		ssize_t n = read(fd, buf + len, sizeof(buf) - 1 - len);
		if (n > 0)
			len += n;
		if (n == 0 || (len && buf[len - 1] == '\n'))
			break;
		ui_test_wait(16);
	}
	buf[len] = '\0';
	return buf;
}

static char *keys(void)
{
	static char buf[4096];
	FILE *f = fopen(KEYS, "r");
	if (!f)
		return NULL;
	buf[fread(buf, 1, sizeof(buf) - 1, f)] = '\0';
	fclose(f);
	return buf;
}

TEST(allowed)
{
	int fd = computer(ED25519 " elian@laptop");
	CHECK(fd >= 0);
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	CHECK(ui_test_shows("elian@laptop"));
	CHECK(ui_test_shows(ED25519_FP));
	CHECK_STR(answer(fd, 0), "");
	CHECK(ui_test_tap_text("Allow"));
	CHECK_STR(answer(fd, 1000), "allowed\n");
	CHECK_STR(keys(), ED25519 " elian@laptop\n");
	CHECK(!ui_test_shows("Allow this computer?"));
	close(fd);
}

TEST(denied)
{
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	CHECK(ui_test_tap_text("Deny"));
	CHECK_STR(answer(fd, 1000), "denied\n");
	CHECK(keys() == NULL);
	CHECK(!ui_test_shows("Allow this computer?"));
	close(fd);
}

/* Over the lock screen: the computer is plugged in, the phone just woke up */
TEST(over_the_lock_screen)
{
	ui_lock();
	ui_test_wait(500);
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	CHECK(ui_test_tap_text("Allow"));
	CHECK_STR(answer(fd, 1000), "allowed\n");
	CHECK(ui_locked());
	close(fd);
}

/* An RSA key: the fingerprint of a blob longer than SHA-256's block */
TEST(rsa_fingerprint)
{
	int fd = computer(RSA " ci@builder");
	CHECK(ui_test_wait_for(RSA_FP, 1000));
	CHECK(ui_test_shows("ci@builder"));
	close(fd);
}

/* No comment after the key: the fingerprint alone */
TEST(without_comment)
{
	int fd = computer(ED25519);
	CHECK(ui_test_wait_for(ED25519_FP, 1000));
	CHECK(ui_test_tap_text("Allow"));
	CHECK_STR(answer(fd, 1000), "allowed\n");
	CHECK_STR(keys(), ED25519 "\n");
	close(fd);
}

/* Kept with the keys there, another computer's without a newline */
TEST(added_to_the_others)
{
	CHECK(ui_test_write(KEYS, RSA " ci@builder"));
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	CHECK(ui_test_tap_text("Allow"));
	CHECK_STR(answer(fd, 1000), "allowed\n");
	CHECK_STR(keys(), RSA " ci@builder\n" ED25519 " elian@laptop\n");
	close(fd);
}

/* A key already allowed, whatever its comment: no question */
TEST(already_allowed)
{
	CHECK(ui_test_write(KEYS, ED25519 " elian@laptop\n"));
	int fd = computer(ED25519 " elian@other");
	CHECK_STR(answer(fd, 1000), "allowed\n");
	CHECK(!ui_test_shows("Allow this computer?"));
	CHECK_STR(keys(), ED25519 " elian@laptop\n");
	close(fd);
}

TEST(invalid)
{
	const char *const lines[] = {
		"hello",
		"ssh-ed25519 not-base64!",
		"ssh-rsa AAAAC3NzaC1lZDI1NTE5AAAAIFDfx3gDrM25JFBgrXqcFOLxTCIe3PTMiMSU2kr6SQQM", /* the blob says ssh-ed25519 */
		"ssh-ed25519 AAAA",
		"ssh-dss AAAAB3NzaC1kc3MAAACBAP", /* a type dropbear refuses */
		ED25519 " elian@laptop\nssh-rsa", /* a second line */
		"command=\"rm -rf /\" " ED25519, /* options */
	};
	for (unsigned i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
		int fd = computer(lines[i]);
		CHECK_STR(answer(fd, 1000), "invalid\n");
		close(fd);
	}
	CHECK(!ui_test_shows("Allow this computer?"));
	CHECK(keys() == NULL);
}

/* The computer gave up (Ctrl+C): the question goes */
TEST(computer_gone)
{
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	close(fd);
	ui_test_wait(500);
	CHECK(!ui_test_shows("Allow this computer?"));
	CHECK(keys() == NULL);
}

/* Unanswered for a minute: denied */
TEST(unanswered)
{
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	ui_test_wait(55000);
	CHECK(ui_test_shows("Allow this computer?"));
	CHECK_STR(answer(fd, 6000), "denied\n");
	CHECK(!ui_test_shows("Allow this computer?"));
	CHECK(keys() == NULL);
	close(fd);
}

/* One question at a time; the next computer can ask once it is answered */
TEST(busy)
{
	int first = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	int second = computer(RSA " ci@builder");
	CHECK_STR(answer(second, 1000), "busy\n");
	CHECK(ui_test_shows("elian@laptop"));
	close(second);
	CHECK(ui_test_tap_text("Deny"));
	CHECK_STR(answer(first, 1000), "denied\n");
	close(first);

	second = computer(RSA " ci@builder");
	CHECK(ui_test_wait_for("ci@builder", 1000));
	close(second);
}

/* A computer that connects and says nothing does not hold the port */
TEST(silent_computer)
{
	int fd = computer("");
	CHECK_STR(answer(fd, 1000), "invalid\n");
	close(fd);

	int silent = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(PORT) };
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	CHECK(connect(silent, (struct sockaddr *)&addr, sizeof(addr)) == 0);
	fcntl(silent, F_SETFL, O_NONBLOCK);
	fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	CHECK_STR(answer(silent, 6000), ""); /* closed */
	close(silent);
	close(fd);
}

/* Settings › Developer mode › Allowed computers: the keys, revoked one at a time */

#define DEV "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEFX/HQlRtWYvzI7RLgDviQcq66I5To5hvAg3xnwyw3J pinephone-dev" /* keys/pinephone, allowed as any other */
#define DEV_FP "SHA256:JxqHmW22c1sG/+OPM72GuKSsP7gsg4bCx+3sAocv0uk"

/* Three computers allowed */
static void three_computers(void)
{
	CHECK(ui_test_write(KEYS, DEV "\n" RSA " ci@builder\n" ED25519 " elian@laptop\n"));
}

static bool open_computers(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Developer mode") &&
	       ui_test_tap_text("Allowed computers");
}

TEST(computers_listed)
{
	three_computers();
	CHECK(ui_test_open("Settings"));
	CHECK(ui_test_tap_text("Developer mode"));
	CHECK(ui_test_shows("Allowed computers"));
	CHECK(ui_test_shows("3"));
	CHECK(ui_test_tap_text("Allowed computers"));
	CHECK(ui_test_shows("elian@laptop"));
	CHECK(ui_test_shows(ED25519_FP));
	CHECK(ui_test_shows("ci@builder"));
	CHECK(ui_test_shows(RSA_FP));
	CHECK(ui_test_shows("pinephone-dev"));
	CHECK(ui_test_shows(DEV_FP));
	CHECK(!ui_test_shows("Built this software"));
}

TEST(computer_revoked)
{
	three_computers();
	CHECK(open_computers());
	CHECK(ui_test_tap_text("elian@laptop"));
	CHECK(ui_test_tap_text("Revoke access"));
	CHECK_STR(keys(), DEV "\n" RSA " ci@builder\n");
	CHECK(!ui_test_shows("elian@laptop"));
	CHECK(ui_test_shows("ci@builder"));

	/* Asked again next time */
	int fd = computer(ED25519 " elian@laptop");
	CHECK(ui_test_wait_for("Allow this computer?", 1000));
	close(fd);
}

TEST(revoke_cancelled)
{
	three_computers();
	CHECK(open_computers());
	CHECK(ui_test_tap_text("ci@builder"));
	CHECK(ui_test_shows("Revoke access"));
	CHECK(ui_test_tap_text("Cancel"));
	CHECK(!ui_test_shows("Revoke access"));
	CHECK(ui_test_shows("ci@builder"));
	CHECK_STR(keys(), DEV "\n" RSA " ci@builder\n" ED25519 " elian@laptop\n");
}

/* The computer that built the software is not trusted apart: revoked as any other */
TEST(build_computer_revoked)
{
	three_computers();
	CHECK(open_computers());
	CHECK(ui_test_tap_text("pinephone-dev"));
	CHECK(ui_test_tap_text("Revoke access"));
	CHECK_STR(keys(), RSA " ci@builder\n" ED25519 " elian@laptop\n");
}

/* Lines the list keeps as they are: comments, options before a key, a key without a name */
TEST(other_lines_kept)
{
	CHECK(ui_test_write(KEYS, "# a comment\nno-pty " RSA " ci@builder\n" ED25519 "\n"));
	CHECK(open_computers());
	CHECK(ui_test_shows("ci@builder"));
	CHECK(ui_test_shows("Unnamed computer"));
	CHECK(ui_test_tap_text("Unnamed computer"));
	CHECK(ui_test_tap_text("Revoke access"));
	CHECK_STR(keys(), "# a comment\nno-pty " RSA " ci@builder\n");
}

TEST(no_computers)
{
	CHECK(open_computers());
	CHECK(ui_test_shows("No computers allowed"));
}
