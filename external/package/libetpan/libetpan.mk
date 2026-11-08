################################################################################
#
# libetpan
#
################################################################################

LIBETPAN_VERSION = 1.10.1
LIBETPAN_SITE = $(call github,dinhviethoa,libetpan,$(LIBETPAN_VERSION))
LIBETPAN_LICENSE = BSD-3-Clause
LIBETPAN_LICENSE_FILES = COPYRIGHT
LIBETPAN_INSTALL_STAGING = YES
# The Email app carries it (apps/email/Makefile), as Telegram carries TDLib
LIBETPAN_INSTALL_TARGET = NO
# A GitHub tarball: no configure script in it
LIBETPAN_AUTORECONF = YES
LIBETPAN_DEPENDENCIES = openssl host-pkgconf

# gnu17: its sources predate C23's defaults (as nixpkgs builds it). No SASL (LOGIN
# over TLS), no database or lock files (no local mailboxes), no cURL or expat
# (no feed or web storage); iconv (glibc's) for the charsets by default: not asked for, as
# configure.ac turns it off on --enable-iconv as on --disable-iconv (its AC_ARG_ENABLE's
# action is enable_iconv=no)
LIBETPAN_CONF_ENV = CFLAGS="$(TARGET_CFLAGS) -std=gnu17"
LIBETPAN_CONF_OPTS = \
	--with-openssl=$(STAGING_DIR)/usr \
	--without-gnutls \
	--without-sasl \
	--without-curl \
	--without-expat \
	--without-zlib \
	--disable-db \
	--disable-lmdb \
	--disable-lockfile \
	--disable-debug \
	--enable-threads

$(eval $(autotools-package))
