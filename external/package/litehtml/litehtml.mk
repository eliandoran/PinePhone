################################################################################
#
# litehtml
#
################################################################################

# master after 0.10: its "Rendering optimization" (2026-10-25) lays flex items out once,
# from their intrinsic sizes, where 0.10 laid each out several times at every level of
# nesting (triliumnotes.org: 8.6 s on the phone)
LITEHTML_VERSION = f358a56a5d537e6f815e267fe8cab5cca1c742cb
LITEHTML_SITE = $(call github,litehtml,litehtml,$(LITEHTML_VERSION))
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
