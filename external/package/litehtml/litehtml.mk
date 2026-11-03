################################################################################
#
# litehtml
#
################################################################################

LITEHTML_VERSION = 0.10
LITEHTML_SITE = $(call github,litehtml,litehtml,v$(LITEHTML_VERSION))
LITEHTML_LICENSE = BSD-3-Clause
LITEHTML_LICENSE_FILES = LICENSE
LITEHTML_INSTALL_STAGING = YES
# Static library only: nothing to install on the target
LITEHTML_INSTALL_TARGET = NO

LITEHTML_CONF_OPTS = \
	-DBUILD_SHARED_LIBS=OFF \
	-DLITEHTML_BUILD_TESTING=OFF \
	-DCMAKE_CXX_FLAGS="$(TARGET_CXXFLAGS) -ffunction-sections -fdata-sections" \
	-DCMAKE_C_FLAGS="$(TARGET_CFLAGS) -ffunction-sections -fdata-sections"

$(eval $(cmake-package))
