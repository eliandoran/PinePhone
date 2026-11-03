################################################################################
#
# tdlib
#
################################################################################

# TDLib only tags rarely; this commit is version 1.8.69
TDLIB_VERSION = 4c6377aa7386c39217878a9a56d581922ad37ca4
TDLIB_SITE = $(call github,tdlib,td,$(TDLIB_VERSION))
TDLIB_LICENSE = BSL-1.0
TDLIB_LICENSE_FILES = LICENSE_1_0.txt
TDLIB_INSTALL_STAGING = YES
TDLIB_SUPPORTS_IN_SOURCE_BUILD = NO
TDLIB_DEPENDENCIES = host-gperf openssl zlib

TDLIB_CONF_OPTS = \
	-DTD_ENABLE_LTO=OFF \
	-DTD_INSTALL_STATIC_LIBRARIES=OFF

# Cross builds do not generate the TL and MIME sources: build the generators
# for the host first, they write into the source tree
define TDLIB_GENERATE_SOURCES
	$(HOST_MAKE_ENV) $(BR2_CMAKE) -S $(@D) -B $(@D)/build-generate \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER="$(HOSTCC_NOCCACHE)" \
		-DCMAKE_CXX_COMPILER="$(HOSTCXX_NOCCACHE)" \
		-DTD_GENERATE_SOURCE_FILES=ON
	$(HOST_MAKE_ENV) $(BR2_CMAKE) --build $(@D)/build-generate -j$(PARALLEL_JOBS)
endef
TDLIB_PRE_CONFIGURE_HOOKS += TDLIB_GENERATE_SOURCES

# Some TDLib sources need over 1 GB each to compile: cap the jobs, and build
# only the shared JSON library
TDLIB_JOBS = $(shell echo $$(( $(PARALLEL_JOBS) < 12 ? $(PARALLEL_JOBS) : 12 )))

define TDLIB_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(BR2_CMAKE) --build $(TDLIB_BUILDDIR) \
		-j$(TDLIB_JOBS) --target tdjson
endef

define TDLIB_INSTALL_STAGING_CMDS
	$(INSTALL) -d $(STAGING_DIR)/usr/lib $(STAGING_DIR)/usr/include/td/telegram
	cp -a $(TDLIB_BUILDDIR)/libtdjson.so* $(STAGING_DIR)/usr/lib/
	$(INSTALL) -m 0644 $(@D)/td/telegram/td_json_client.h \
		$(TDLIB_BUILDDIR)/td/telegram/tdjson_export.h \
		$(STAGING_DIR)/usr/include/td/telegram/
endef

define TDLIB_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/lib
	cp -a $(TDLIB_BUILDDIR)/libtdjson.so* $(TARGET_DIR)/usr/lib/
endef

$(eval $(cmake-package))
