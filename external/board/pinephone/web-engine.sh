#!/bin/sh
# The web engine (WPE WebKit and what only it uses) is built by Buildroot but not
# shipped in the image: the Web plugin app (apps/web) carries it in
# /var/lib/apps/web/root/, laid out as in Buildroot's target folder.
#
#   web-engine.sh image TARGET BUILD_DIR READELF
#       post-fakeroot, on the image's copy of the target folder: removes the engine,
#       links its compiled-in folders (ENGINE_DIRS) into the app's root/, and fails
#       if a program or library left needs a library that went with it
#   web-engine.sh bundle TARGET BUILD_DIR OUT PATCHELF
#       copies the engine's libraries and ENGINE_DIRS from TARGET into OUT, each ELF
#       file with a runpath to OUT/usr/lib
#
# The engine's packages are found in Buildroot's packages-file-list.txt; WebKit's
# own files by name (a wpewebkit-rebuild drops its entries from that list).

set -e

APP=/var/lib/apps/web/root

# Only WebKit needs them: the image keeps what Mesa (Wayland, expat), curl (OpenSSL,
# zlib), phone-ui (webp: Web Lite's pictures) and the rest use
ENGINE_PACKAGES="libwpe wpebackend-fdo libsoup3 glib-networking libglib2 icu harfbuzz
	fontconfig freetype dejavu libepoxy libgcrypt libgpg-error libtasn1 libxml2 libxslt
	sqlite libpsl nghttp2 libxkbcommon pcre2"
WEBKIT_LIBS="usr/lib/libWPEWebKit-2.0.so*"
# Tools of other packages that need the engine's libraries, useless on the phone:
# Wayland's code generator (libxml2)
TOOLS="usr/bin/wayland-scanner"

# Folders the engine reads at paths compiled into it: in the app, linked from the
# image (with the fontconfig settings post-build.sh and the overlay made)
ENGINE_DIRS="etc/fonts usr/lib/gio usr/lib/wpe-webkit-2.0 usr/libexec/wpe-webkit-2.0
	usr/share/fontconfig usr/share/fonts usr/share/glib-2.0 usr/share/wpe-webkit-2.0
	usr/share/xml"

# The engine's files, relative to TARGET (a package's files that are gone are left out)
engine_files() {
	target=$1 list=$2/packages-file-list.txt
	for p in $ENGINE_PACKAGES; do
		grep "^$p," "$list" | cut -d, -f2- | sed 's#^\./##'
	done | while read -r f; do
		[ -e "$target/$f" ] || [ -L "$target/$f" ] && echo "$f"
	done
	(cd "$target" && for f in $WEBKIT_LIBS; do [ -e "$f" ] && echo "$f"; done)
}

# Libraries directly in lib/ or usr/lib/ (the engine's tools, gettext and ICU's
# build files are not needed)
is_library() {
	case $1 in
	lib/*/* | usr/lib/*/*) return 1 ;;
	lib/*.so* | usr/lib/*.so*) return 0 ;;
	*) return 1 ;;
	esac
}

image() {
	target=${1:?} build=$2 readelf=$3
	for f in $TOOLS; do
		rm -f "${target:?}/$f"
	done
	engine_files "$target" "$build" | while read -r f; do
		rm -f "${target:?}/${f:?}"
	done
	for d in $ENGINE_DIRS; do
		rm -rf "${target:?}/$d"
		mkdir -p "$(dirname "$target/$d")"
		ln -s "$APP/$d" "$target/$d"
	done

	# Every ELF file left must find its libraries in the image. Not glibc's gconv
	# modules, loaded for an encoding when asked: Buildroot copies some without
	# those they need (ARIB-STD-B24 without libJIS), with or without the engine.
	missing=$(find "$target" -path "$target/usr/lib/gconv" -prune -o -type f \
		\( -perm -u+x -o -name '*.so*' \) -print | while read -r f; do
		[ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] || continue
		"$readelf" -d "$f" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' |
		while read -r lib; do
			[ -e "$target/lib/$lib" ] || [ -e "$target/usr/lib/$lib" ] ||
				[ -e "$(dirname "$f")/$lib" ] || # next to it ($ORIGIN)
				echo "${f#"$target"/} needs $lib"
		done
	done)
	if [ -n "$missing" ]; then
		echo "web-engine.sh: the image lacks libraries that went with the web engine:" >&2
		echo "$missing" >&2
		exit 1
	fi
}

bundle() {
	target=$1 build=$2 out=$3 patchelf=$4
	mkdir -p "$out"
	files=$(engine_files "$target" "$build" | while read -r f; do
		is_library "$f" && echo "$f"
	done)
	# shellcheck disable=SC2086 # word splitting: the file and folder lists
	(cd "$target" && cp -a --parents $files $ENGINE_DIRS "$out")

	# Each program and library finds the others through a runpath of its own: some
	# have an empty one, which would hide the rpath of what loads them
	find "$out" -type f | while read -r f; do
		[ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = "177ELF" ] || continue
		rel=$(realpath -m --relative-to="$(dirname "$f")" "$out/usr/lib")
		"$patchelf" --set-rpath "\$ORIGIN/$rel" "$f"
	done
}

case $1 in
image) shift; image "$@" ;;
bundle) shift; bundle "$@" ;;
*) echo "usage: $0 image TARGET BUILD_DIR READELF | bundle TARGET BUILD_DIR OUT PATCHELF" >&2; exit 2 ;;
esac
