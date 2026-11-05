################################################################################
#
# gpiod-tools: Buildroot's libgpiod2 without its kernel headers dependency
# (Config.in), the C library and its tools only
#
################################################################################

GPIOD_TOOLS_VERSION = 2.3.1
GPIOD_TOOLS_SOURCE = libgpiod-$(GPIOD_TOOLS_VERSION).tar.xz
GPIOD_TOOLS_SITE = https://www.kernel.org/pub/software/libs/libgpiod
GPIOD_TOOLS_DL_SUBDIR = libgpiod2
GPIOD_TOOLS_LICENSE = LGPL-2.1+ (library), GPL-2.0+ (tools)
GPIOD_TOOLS_LICENSE_FILES = COPYING
GPIOD_TOOLS_DEPENDENCIES = host-pkgconf
GPIOD_TOOLS_CONF_OPTS = \
	-Dtools=enabled \
	-Dgpioset-interactive=disabled \
	-Dbindings-cxx=disabled \
	-Dbindings-glib=disabled \
	-Dbindings-python=disabled \
	-Dbindings-rust=disabled \
	-Ddbus=disabled \
	-Dintrospection=disabled \
	-Dsystemd=disabled \
	-Dexamples=disabled \
	-Dtests=disabled

$(eval $(meson-package))
