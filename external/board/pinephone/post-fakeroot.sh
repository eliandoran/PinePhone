#!/bin/sh
# Runs on the image's copy of the target folder (Buildroot's own stays whole): the
# web engine goes to the Web plugin app instead (web-engine.sh).

set -e

if grep -q '^BR2_PACKAGE_WPEWEBKIT=y' "$BR2_CONFIG"; then
	"$(dirname "$0")/web-engine.sh" image "$1" "$BUILD_DIR" "$HOST_DIR/bin/aarch64-linux-readelf"
fi
