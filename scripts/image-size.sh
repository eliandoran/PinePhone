#!/usr/bin/env bash
# What the root filesystem image is made of: its files' sizes per Buildroot package.
#
# Usage: image-size.sh [--files N] [IMAGE]
#   IMAGE       the ext4 image (default: output/images/rootfs.ext2)
#   --files N   also list the N largest files
#
# Read from the image itself with Buildroot's debugfs (no mount, no sudo), so what
# post-fakeroot.sh leaves out (the web engine) is not counted. A file belongs to the
# last package that installed it: each package's output/build/<pkg>/.files-list.txt
# (packages-file-list.txt gathers them only at the end of a full build). The others
# are grouped by where they come from. Sizes are the files' own, hard links once.

set -euo pipefail

out=output
files=0
image=
while [ $# -gt 0 ]; do
	case $1 in
	--files) files=$2; shift 2 ;;
	*) image=$1; shift ;;
	esac
done
image=${image:-$out/images/rootfs.ext2}
debugfs=$out/host/sbin/debugfs
overlay=external/board/pinephone/rootfs-overlay
for f in "$image" "$debugfs"; do
	[ -e "$f" ] || { echo "image-size: $f not found (just build first)" >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Every file with its inode and size, one directory level per debugfs run: the
# "ls -p" lines are /inode/mode/uid/gid/name/size/
echo / > "$tmp/dirs"
: > "$tmp/files"
while [ -s "$tmp/dirs" ]; do
	sed 's/.*/ls -p "&"/' "$tmp/dirs" > "$tmp/cmds"
	"$debugfs" -f "$tmp/cmds" "$image" 2>/dev/null | awk -F/ -v files="$tmp/files" '
		/^debugfs: ls -p / { dir = substr($0, 16); gsub(/"/, "", dir); sub(/\/$/, "", dir); next }
		NF >= 7 && $6 != "." && $6 != ".." {
			path = dir "/" $6
			if ($3 ~ /^04/) print path
			else if ($3 ~ /^10/) print $2, $7, substr(path, 2) >> files
		}' > "$tmp/next"
	mv "$tmp/next" "$tmp/dirs"
done

(cd "$overlay" && find . -type f | sed 's|^\./||') > "$tmp/overlay"
cat "$out"/build/*/.files-list.txt > "$tmp/owners"
used=$("$debugfs" -R stats "$image" 2>/dev/null |
	awk '/^Block count:/ { b = $3 } /^Free blocks:/ { f = $3 } /^Block size:/ { s = $3 } END { print (b - f) * s }')

awk -v files_n="$files" -v image="$image" -v used="$used" -v tmp="$tmp" '
	FILENAME == tmp "/overlay" { ovl[$0] = 1; next }
	FILENAME == tmp "/owners" {
		i = index($0, ","); f = substr($0, i + 1); sub(/^\.\//, "", f)
		owner[f] = substr($0, 1, i - 1); next
	}
	{
		ino = $1; size = $2; path = substr($0, length($1) + length($2) + 3)
		if (ino in seen) next
		seen[ino] = 1
		if (path in ovl) group = "(rootfs overlay)"
		else if (path in owner) group = owner[path]
		else if (path ~ /^boot\//) group = "(kernel and device trees)"
		else if (path ~ /^lib\/modules\//) group = "(kernel modules)"
		else group = "(no package)"
		sum[group] += size; count[group]++; total += size
		if (files_n > 0) print size, path > (tmp "/sizes")
	}
	END {
		printf "%s: %.1f MB in %d files, %.1f MB used with the filesystem'"'"'s own\n\n", \
			image, total / 1048576, length(seen), used / 1048576
		for (g in sum) printf "%8.2f MB %5.1f%% %6d  %s\n", sum[g] / 1048576, 100 * sum[g] / total, count[g], g | "sort -rn"
	}' "$tmp/overlay" "$tmp/owners" "$tmp/files"

if [ "$files" -gt 0 ]; then
	echo
	sort -rn "$tmp/sizes" | awk -v n="$files" 'NR <= n { s = $1; $1 = ""; printf "%8.2f MB %s\n", s / 1048576, substr($0, 2) }'
fi
