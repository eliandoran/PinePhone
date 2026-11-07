/* docs/external/terminal.md: Terminal, the plugin app, its shell a script that prints a few
 * commands and their output as BusyBox's would, then waits */

#include <sys/stat.h>

#define TERMINAL_SCRIPT "/tmp/terminal-shell" /* TERMINAL_SHELL of the tests' terminal.so */

static void terminal_seed(void)
{
	plugin_install(TERMINAL_APP, "terminal", "terminal.so", NULL);
	ui_test_write(TERMINAL_SCRIPT,
	              "#!/bin/sh\n"
	              "printf '%s\\n' \\\n"
	              "  '~ # uptime' \\\n"
	              "  ' 09:41:00 up 2 days,  3:12,  load average: 0.12, 0.20, 0.18' \\\n"
	              "  '~ # cat /sys/class/power_supply/axp20x-battery/capacity' \\\n"
	              "  '76' \\\n"
	              "  '~ # df -h /data' \\\n"
	              "  'Filesystem                Size      Used Available Use%' \\\n"
	              "  '/dev/mmcblk2p3           27.9G      3.1G     23.4G  12%' \\\n"
	              "  '~ # ls /var/lib' \\\n"
	              "  'apps       books      messages   music      notes' \\\n"
	              "  'phone-ui   photos     telegram   wpa_supplicant'\n"
	              "printf '~ # '\n"
	              "read line\n");
	chmod(TERMINAL_SCRIPT, 0755);
}

/* terminal.md: a few commands, the keyboard and the row of keys over it */
TEST(terminal_commands)
{
	CHECK(ui_test_open("Terminal"));
	CHECK(ui_test_wait_for("76", 2000));
	shot("terminal");
}
