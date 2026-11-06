include $(sort $(wildcard $(BR2_EXTERNAL_PINEPHONE_PATH)/package/*/*.mk))

# XML::Parser builds with perl's own link flags, without Buildroot's rpath: its
# Makefile.PL check and the Expat module it builds would not find host-expat's
# libexpat (LDDLFLAGS reaches the build as a make variable)
HOST_LIBXML_PARSER_PERL_CONFIGURE_OPTS += \
	LD_LIBRARY_PATH=$(HOST_DIR)/lib \
	LDDLFLAGS="-shared -O2 -Wl,-rpath,$(HOST_DIR)/lib"

# curl uses libpsl and nghttp2 when they are built, and they only are for WebKit:
# libpsl would bring ICU into the image (web-engine.sh). The last option wins.
LIBCURL_CONF_OPTS += --without-libpsl --without-nghttp2

# WebKit's unified C++ sources need 2-4 GB each to compile: 10 jobs stay within the
# 32 GB the justfile gives builds, a job per 4 GB of RAM on a smaller host (a 30 GB WSL
# VM ran out with 10, before the cap). Passed to ninja after cmake's -j, the last one wins.
WPEWEBKIT_JOBS := $(shell awk '/^MemTotal:/ { n = int($$2 / 4194304); print (n < 1 ? 1 : n > 10 ? 10 : n) }' /proc/meminfo)
WPEWEBKIT_BUILD_OPTS = -- -j$(WPEWEBKIT_JOBS)

# fontconfig's cache in /var/cache (RAM here) is rebuilt on every boot: about 6 s on
# the first page WebKit draws. On the data partition it is built once.
FONTCONFIG_CONF_OPTS += -Dcache-dir=/var/lib/fontconfig

# Boot logo: phone-ui's splash (gen-splash.sh), drawn by fbcon in the middle of the
# screen (fbcon=logo-pos:center in extlinux.conf) until phone-ui's first frame
define LINUX_SPLASH_LOGO
	cmp -s $(BR2_EXTERNAL_PINEPHONE_PATH)/package/phone-ui/splash.ppm \
		$(@D)/drivers/video/logo/logo_linux_clut224.ppm || \
	cp $(BR2_EXTERNAL_PINEPHONE_PATH)/package/phone-ui/splash.ppm \
		$(@D)/drivers/video/logo/logo_linux_clut224.ppm
endef
LINUX_PRE_BUILD_HOOKS += LINUX_SPLASH_LOGO

# Bluetooth firmware of the RTL8723CS (Wi-Fi and Bluetooth share its antenna): in
# linux-firmware, not in Buildroot's lists of it. Without it, Bluetooth stays half
# initialised.
define LINUX_FIRMWARE_INSTALL_RTL8723CS_BT
	$(INSTALL) -D -m 0644 $(@D)/rtl_bt/rtl8723cs_xx_fw.bin \
		$(TARGET_DIR)/lib/firmware/rtl_bt/rtl8723cs_xx_fw.bin
	$(INSTALL) -D -m 0644 $(@D)/rtl_bt/rtl8723cs_xx_config.bin \
		$(TARGET_DIR)/lib/firmware/rtl_bt/rtl8723cs_xx_config.bin
endef
LINUX_FIRMWARE_POST_INSTALL_TARGET_HOOKS += LINUX_FIRMWARE_INSTALL_RTL8723CS_BT
