#!/usr/bin/env bash
# One step of CI's image build (.github/workflows/image.yml): a Buildroot target made in the
# FHS environment, then its row in the run's summary: how long it took and what ccache did
# for it (its counters zeroed first, so each step's are its own).
#
#   ci-build-step.sh <target>   make it, stopped at BUILD_DEADLINE (seconds since the epoch)
#   ci-build-step.sh --stop     kill what a build left running
#
# The deadline is ours, ahead of the job's: a step GitHub times out or cancels may leave
# compilers running, and the ccache they still write to fails to save ("file changed as we
# read it": five hours of WebKit lost once).

set -u

ccache=output/host/bin/ccache
export CCACHE_DIR=$HOME/.buildroot-ccache

# Whatever still runs from the build's trees, compilers and ccache above all
stop() {
    local i
    for i in $(seq 30); do
        pkill -KILL -f "$PWD/(output|buildroot)/" || return 0
        sleep 1
    done
    echo "::warning::Build processes still running after 30 s"
}

stat() {
    "$ccache" --print-stats | awk -F '\t' -v key="$1" '$1 == key { print $2 }'
}

if [ "${1:-}" = --stop ]; then
    stop
    exit 0
fi

target=${1:?target}
start=$(date +%s)
left=$(( ${BUILD_DEADLINE:-$(( start + 5 * 3600 ))} - start ))

counted=
result="out of time"
status=1
if [ "$left" -gt 0 ]; then
    # Not there before host-ccache is built: the first step has no counters
    if [ -x "$ccache" ] && "$ccache" --zero-stats > /dev/null; then
        counted=1
    fi
    timeout --signal=TERM --kill-after=30 "$left" nix run .#fhs -- -c "make $target"
    status=$?
    case $status in
        0) result="done" ;;
        124 | 137) result="out of time" ;;
        *) result="failed" ;;
    esac
    [ "$status" -eq 0 ] || stop
fi

took=$(( $(date +%s) - start ))
hits=- misses=- size=-
if [ -n "$counted" ]; then
    hits=$(( $(stat direct_cache_hit) + $(stat preprocessed_cache_hit) ))
    misses=$(stat cache_miss)
fi
[ -d "$CCACHE_DIR" ] && size=$(du -sh "$CCACHE_DIR" | cut -f1)

row="| \`make $target\` | $result | $(( took / 3600 ))h $(( took % 3600 / 60 ))m | $hits | $misses | $size |"
echo "$row"
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && echo "$row" >> "$GITHUB_STEP_SUMMARY"
# For the workflow: whether the step compiled anything worth saving
[ -n "${GITHUB_OUTPUT:-}" ] && echo "misses=$misses" >> "$GITHUB_OUTPUT"

exit "$status"
