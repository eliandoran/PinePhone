/*
 * Installing on the phone (Settings › Storage, emmc_install.c) from a microSD card, with
 * the board's emmc-install.sh itself, on files of the test's in /tmp for the disks: the
 * card, the slot running from it (an ext4 image), the eMMC (sparse, 1100 MiB: the data
 * partition 72 of them), its boot0, the card's data partition (a folder) and Tow-Boot's
 * build for boot0. What was written is read back with sfdisk and debugfs.
 */

#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test.h"
#include "ui_test.h"

#define D "/tmp/disks"
#define EMMC D "/emmc"
#define BOOT0 D "/boot0"
#define CARD D "/card"
#define TOW_BOOT D "/tow-boot.bin"
#define SLOT_A_FS EMMC "?offset=4194304"
#define DATA_FS EMMC "?offset=1077936128" /* sector 2105344 */
/* Its steps held while this is there: mke2fs (the data's) waits for it */
#define HOLD "/tmp/hold"

/* A command's output (static), "" if none */
static const char *out(const char *cmd)
{
	static char text[8192];
	size_t n = 0;
	FILE *f = popen(cmd, "r");
	if (f) {
		n = fread(text, 1, sizeof(text) - 1, f);
		pclose(f);
	}
	text[n] = '\0';
	return text;
}

static bool sh(const char *cmd)
{
	int r = system(cmd);
	if (r != 0)
		fprintf(stderr, "  failed (%d): %s\n", r, cmd);
	return r == 0;
}

/* Running from slot `slot` of a card (signature 50480002) or of the eMMC (50480001) */
static void boot(const char *signature, int slot)
{
	char line[128];
	snprintf(line, sizeof(line), "console=ttyS0 root=PARTUUID=%s-0%d rootwait ro\n", signature, slot);
	ui_test_write(D "/cmdline", line);
}

TEST_SETUP
{
	ui_test_reset();
	CHECK(sh("set -e; rm -rf " D " /tmp/hold /tmp/hold-bin /tmp/fail-bin /tmp/tools; mkdir -p " D "/root/boot/extlinux " D "/data/var/lib/notes " D "/data/var/lib/pp-update\n"
	         "cd " D "\n"
	         "printf 'label buildroot\\n\\tkernel /boot/Image\\n\\tappend root=PARTUUID=50480002-02 rootwait ro\\n'"
	         " > root/boot/extlinux/extlinux.conf\n"
	         "echo 'the system' > root/hello\n"
	         "mke2fs -q -F -t ext4 -d root root.ext4 8M\n"
	         "echo 'Shopping' > data/var/lib/notes/list.txt\n"
	         "truncate -s 1100M emmc; truncate -s 4M boot0\n"
	         /* another system's: its partition table, a bootloader at 8 KiB, a GPT's headers */
	         "printf 'label: dos\\nlabel-id: 0x12345678\\nstart=2048, type=83, bootable\\n' | sfdisk -q emmc\n"
	         "printf 'XXXXeGON.BT0' | dd of=emmc bs=1 seek=8192 conv=notrunc 2>/dev/null\n"
	         "printf 'EFI PART' | dd of=emmc bs=512 seek=1 conv=notrunc 2>/dev/null\n"
	         "printf 'EFI PART' | dd of=emmc bs=512 seek=$((1100 * 2048 - 1)) conv=notrunc 2>/dev/null\n"
	         /* the card: its table, Tow-Boot at 8 KiB */
	         "head -c 65536 /dev/urandom > card\n"
	         "printf 'XXXXeGON.BT0' | dd of=card bs=1 seek=8192 conv=notrunc 2>/dev/null\n"
	         "head -c 20000 /dev/urandom > tow-boot.bin\n"
	         "printf 'XXXXeGON.BT0' | dd of=tow-boot.bin bs=1 conv=notrunc 2>/dev/null\n"));
	boot("50480002", 2);
	setenv("CMDLINE", D "/cmdline", 1);
	setenv("CARD", CARD, 1);
	setenv("ROOT", D "/root.ext4", 1);
	setenv("EMMC", EMMC, 1);
	setenv("BOOT0", BOOT0, 1);
	setenv("DATA", D "/data", 1);
	setenv("TOW_BOOT", TOW_BOOT, 1);
	setenv("PROC", "/tmp/proc", 1);
}

/* Its data step waits while HOLD is there, then runs the real mke2fs */
static void hold(void)
{
	char real[1024], script[1200], path[4096];
	snprintf(real, sizeof(real), "%s", out("command -v mke2fs"));
	real[strcspn(real, "\n")] = '\0';
	CHECK(real[0] == '/');
	snprintf(script, sizeof(script), "#!/bin/sh\nwhile [ -e " HOLD " ]; do sleep 0.01; done\nexec %s \"$@\"\n", real);
	ui_test_write(HOLD, "");
	ui_test_write("/tmp/hold-bin/mke2fs", script);
	CHECK(sh("chmod +x /tmp/hold-bin/mke2fs"));
	snprintf(path, sizeof(path), "/tmp/hold-bin:%s", getenv("PATH"));
	setenv("PATH", path, 1);
}

/* The data's step in progress: the two before it done */
static bool at_data(void)
{
	lv_obj_t *found[4];
	return ui_test_find_all("Done", found, 4) == 2 && ui_test_shows("Installing...");
}

static bool powered_off(void)
{
	return strstr(ui_test_ran(), "poweroff");
}

static bool storage(void)
{
	return ui_test_open("Settings") && ui_test_tap_text("Storage");
}

static bool install(void)
{
	return storage() && ui_test_tap_text("Install on the phone") && ui_test_tap_text("Erase and install");
}

TEST(offered_from_a_card_only)
{
	boot("50480001", 1);
	CHECK(storage());
	CHECK(ui_test_shows("System"));
	CHECK(!ui_test_shows("Install on the phone"));
}

TEST(not_offered_without_an_emmc)
{
	setenv("EMMC", D "/none", 1);
	CHECK(storage());
	CHECK(!ui_test_shows("Install on the phone"));
}

TEST(says_what_is_erased)
{
	CHECK(storage());
	CHECK(ui_test_tap_text("Install on the phone"));
	CHECK(ui_test_shows("Everything else on the phone's storage is erased: another system, its files, photos "
	                    "and messages."));
	CHECK(ui_test_shows("Erase and install"));
	/* Nothing written before it is tapped */
	CHECK(strstr(out("sfdisk -d " EMMC), "label-id: 0x12345678"));
}

TEST(installs_the_system_bootloader_and_data)
{
	const char *card = out("sha256sum < " CARD);
	char card_sum[128];
	snprintf(card_sum, sizeof(card_sum), "%s", card);

	CHECK(install());
	CHECK(ui_test_wait_for("Installed", 30000));
	CHECK(ui_test_shows("Switch off"));

	/* The partition table pinephone.img has: slot A bootable, B, data to the end */
	const char *table = out("sfdisk -d " EMMC);
	CHECK(strstr(table, "label-id: 0x50480001"));
	CHECK(strstr(table, "start=        8192, size=     1048576, type=83, bootable"));
	CHECK(strstr(table, "start=     1056768, size=     1048576, type=83\n"));
	CHECK(strstr(table, "start=     2105344, size=      147456, type=83\n"));
	/* The other system's GPT and bootloader gone, Tow-Boot in boot0 */
	CHECK_STR(out("dd if=" EMMC " bs=512 skip=1 count=1 2>/dev/null | tr -d '\\000'"), "");
	CHECK_STR(out("dd if=" EMMC " bs=512 skip=$((1100 * 2048 - 1)) count=1 2>/dev/null | tr -d '\\000'"), "");
	CHECK_STR(out("dd if=" EMMC " bs=512 skip=16 count=1 2>/dev/null | tr -d '\\000'"), "");
	CHECK(sh("cmp -n 20000 " BOOT0 " " TOW_BOOT));
	/* The running slot as slot A, pointed at it */
	CHECK_STR(out("debugfs -R 'cat /hello' '" SLOT_A_FS "' 2>/dev/null"), "the system\n");
	CHECK(strstr(out("debugfs -R 'cat /boot/extlinux/extlinux.conf' '" SLOT_A_FS "' 2>/dev/null"),
	             "append root=PARTUUID=50480001-01 rootwait ro\n"));
	/* The data, in the partition S00data mounts */
	CHECK_STR(out("debugfs -R 'cat /var/lib/notes/list.txt' '" DATA_FS "' 2>/dev/null"), "Shopping\n");
	CHECK(strstr(out("blkid --probe --offset 1077936128 " EMMC), "LABEL=\"PP_DATA\""));
	/* The card only read */
	CHECK_STR(out("sha256sum < " CARD), card_sum);
	CHECK(sh("cmp -n 20000 " D "/root.ext4 " EMMC " 0 4194304"));
}

TEST(keeps_a_bootloader_in_boot0)
{
	CHECK(sh("printf 'XXXXeGON.BT0 already' | dd of=" BOOT0 " conv=notrunc 2>/dev/null"));
	CHECK(install());
	CHECK(ui_test_wait_for("Installed", 30000));
	CHECK(ui_test_shows("Kept"));
	CHECK_STR(out("head -c 20 " BOOT0), "XXXXeGON.BT0 already");
	/* The user area's still cleared: it would start before boot0's */
	CHECK_STR(out("dd if=" EMMC " bs=512 skip=16 count=1 2>/dev/null | tr -d '\\000'"), "");
}

TEST(shows_its_steps_and_power_does_not_sleep)
{
	hold();
	CHECK(install());
	CHECK(ui_test_wait_until(at_data, 30000));
	CHECK(ui_test_shows("Keep the card in and the phone on until it is done."));
	CHECK(!ui_test_shows("Erase and install"));

	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(1000);
	CHECK(!ui_locked());

	/* Left and opened again: where it is */
	CHECK(ui_test_back());
	CHECK(ui_test_tap_text("Install on the phone"));
	CHECK(ui_test_shows("Installing..."));

	remove(HOLD);
	CHECK(ui_test_wait_for("Installed", 30000));
	ui_test_button_press(KEY_POWER, 100);
	ui_test_wait(1000);
	CHECK(ui_locked());
}

TEST(fails_and_tries_again)
{
	ui_test_write(D "/data/var/lib/pp-update/trial", "2 1\n");
	CHECK(install());
	CHECK(ui_test_wait_for("Installing failed", 30000));
	CHECK(ui_test_shows("the system is on trial: restart first"));
	CHECK(strstr(out("sfdisk -d " EMMC), "label-id: 0x12345678"));

	remove(D "/data/var/lib/pp-update/trial");
	CHECK(ui_test_tap_text("Try again"));
	CHECK(ui_test_wait_for("Installed", 30000));
	CHECK(strstr(out("sfdisk -d " EMMC), "label-id: 0x50480001"));
}

TEST(a_failed_step_says_so)
{
	/* No file system to copy */
	CHECK(sh("truncate -s 8M " D "/root.ext4 && dd if=/dev/zero of=" D "/root.ext4 bs=1M count=8 2>/dev/null"));
	CHECK(install());
	CHECK(ui_test_wait_for("Installing failed", 30000));
	CHECK(ui_test_shows_part("no file system on"));
	CHECK(ui_test_shows("Try again"));
}

/* An image built without one of its tools (debugfs: a package option not rebuilt) */
TEST(a_missing_tool_is_named_before_anything_is_written)
{
	CHECK(sh("set -e; mkdir -p /tmp/tools; for t in sh sed awk readlink basename dd tr head tail sha256sum cut wc "
	         "du mktemp grep rm sync cat sfdisk mke2fs e2fsck dumpe2fs blockdev cmp; do "
	         "ln -sf \"$(command -v $t)\" /tmp/tools/$t; done"));
	/* Only those (ui_test.c puts its fakes before them) */
	setenv("PATH", "/tmp/tools", 1);
	CHECK(install());
	CHECK(ui_test_wait_for("Installing failed", 30000));
	CHECK(ui_test_shows_part("no debugfs"));
	CHECK(strstr(out("sfdisk -d " EMMC), "label-id: 0x12345678"));
	CHECK_STR(out("dd if=" BOOT0 " bs=512 count=1 2>/dev/null | tr -d '\\000'"), "");
}

/* What the tool said, after which step failed */
TEST(a_tools_error_is_shown)
{
	ui_test_write("/tmp/fail-bin/mke2fs", "#!/bin/sh\necho 'mke2fs: Could not allocate block in ext2 filesystem' >&2\n"
	                                      "exit 1\n");
	CHECK(sh("chmod +x /tmp/fail-bin/mke2fs"));
	char path[4096];
	snprintf(path, sizeof(path), "/tmp/fail-bin:%s", getenv("PATH"));
	setenv("PATH", path, 1);
	CHECK(install());
	CHECK(ui_test_wait_for("Installing failed", 30000));
	CHECK(ui_test_shows("cannot copy the data to " EMMC ": mke2fs: Could not allocate block in ext2 filesystem"));
	CHECK(ui_test_shows("Failed")); /* on Your data */
	/* No partition table yet: Tow-Boot, which looks on the eMMC first, would start its half-written slot */
	CHECK_STR(out("sfdisk -d " EMMC " 2>/dev/null"), "");
}

TEST(switches_off_keeping_the_card)
{
	const char *card = out("sha256sum < " CARD);
	char card_sum[128];
	snprintf(card_sum, sizeof(card_sum), "%s", card);
	CHECK(install());
	CHECK(ui_test_wait_for("Installed", 30000));
	CHECK(ui_test_tap_text("Switch off"));
	CHECK_STR(ui_test_ran(), "poweroff\n");
	CHECK(ui_test_shows("Powering off..."));
	CHECK_STR(out("sha256sum < " CARD), card_sum);
}

TEST(erases_the_card_and_switches_off)
{
	CHECK(install());
	CHECK(ui_test_wait_for("Installed", 30000));
	CHECK(ui_test_tap_text("Erase the card and switch off"));
	CHECK(ui_test_wait_for("Powering off...", 5000));
	CHECK(ui_test_wait_until(powered_off, 10000));
	CHECK_STR(ui_test_ran(), "poweroff\n");
	/* Its partition table and Tow-Boot's header */
	CHECK_STR(out("dd if=" CARD " bs=512 count=1 2>/dev/null | tr -d '\\000'"), "");
	CHECK_STR(out("dd if=" CARD " bs=512 skip=16 count=1 2>/dev/null | tr -d '\\000'"), "");
	/* The rest as it was */
	CHECK(strcmp(out("dd if=" CARD " bs=512 skip=1 count=1 2>/dev/null | tr -d '\\000' | wc -c"), "0\n") != 0);
}
