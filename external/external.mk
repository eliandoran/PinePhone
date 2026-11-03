include $(sort $(wildcard $(BR2_EXTERNAL_PINEPHONE_PATH)/package/*/*.mk))

# perf (linux-tools) builds Rust test workloads when the host has rustc, which
# fails without a Rust target for aarch64
PERF_MAKE_FLAGS += NO_RUST=1

# XML::Parser builds with perl's own link flags, without Buildroot's rpath: its
# Makefile.PL check and the Expat module it builds would not find host-expat's
# libexpat (LDDLFLAGS reaches the build as a make variable)
HOST_LIBXML_PARSER_PERL_CONFIGURE_OPTS += \
	LD_LIBRARY_PATH=$(HOST_DIR)/lib \
	LDDLFLAGS="-shared -O2 -Wl,-rpath,$(HOST_DIR)/lib"

# WebKit's unified C++ sources need 2-3 GB each to compile: 10 jobs stay within the
# 32 GB the justfile gives builds. Passed to ninja after cmake's -j, the last one wins.
WPEWEBKIT_BUILD_OPTS = -- -j10

# fontconfig's cache in /var/cache (RAM here) is rebuilt on every boot: about 6 s on
# the first page WebKit draws. On the data partition it is built once.
FONTCONFIG_CONF_OPTS += -Dcache-dir=/var/lib/fontconfig
