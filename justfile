# Run `nix develop` first to get just and the tools used below.

set positional-arguments

phone := "root@172.16.42.1"
ssh_opts := "-i keys/pinephone -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"

# Builds run in a scope of at most 32 GB of RAM and no swap: past it the kernel kills a
# compiler (the build fails) instead of the host running out of memory
fhs := "systemd-run --user --scope --quiet -p MemoryMax=32G -p MemorySwapMax=0 nix run .#fhs"

default:
    @just --list

# Build the firmware into output/images/
build:
    {{fhs}} -- -c make

# Run a Buildroot target in the build environment, e.g. `just make linux-rebuild`
make *args:
    {{fhs}} -- -c 'make "$@"' make "$@"

# Configure Buildroot, then `just savedefconfig` to keep the changes
menuconfig:
    just make menuconfig

# Configure the kernel (save changes into board/pinephone/linux.fragment by hand)
linux-menuconfig:
    just make linux-menuconfig

# Write the Buildroot configuration back to external/configs/pinephone_defconfig
savedefconfig:
    just make savedefconfig

# Interactive shell in the build environment
shell:
    {{fhs}}

# Keep buildroot/, output/, dl/ and keys/ in another directory and link them here, e.g.
# on WSL's own filesystem when the sources are on Windows (/mnt/c is slow and has no
# Linux permissions); existing ones are moved there
build-dir dir:
    #!/usr/bin/env bash
    set -euo pipefail
    mkdir -p "$1"
    dir=$(realpath "$1")
    for d in buildroot output dl keys; do
        [ -L "$d" ] && { echo "$d -> $(readlink "$d")"; continue; }
        if [ -e "$d" ]; then
            [ ! -e "$dir/$d" ] || { echo "Both $d and $dir/$d exist." >&2; exit 1; }
            mv "$d" "$dir/$d"
        else
            mkdir -p "$dir/$d"
        fi
        ln -s "$dir/$d" "$d"
        echo "$d -> $dir/$d"
    done

# Flash over the air (our firmware running); fails if the phone does not answer
flash *args:
    nix run . -- "$@"

# Flash the full image via USB mass storage (Volume Up at power-on, or FEL)
flash-usb:
    nix run . -- --usb

# Only expose the eMMC as a USB disk, write nothing
expose:
    nix run . -- --expose

# Build and flash
deploy: build flash

# Push phone-ui from the working tree to the running phone, no flash; args are
# passed to it (e.g. `just ui --open Camera`). Kept across reboots until the next
# flash (see S90phone-ui); `just ui-reset` goes back to the installed one.
ui *args:
    #!/usr/bin/env bash
    set -euo pipefail
    # One push at a time (several agents), and never next to a full build
    exec 9>output/.ui.lock
    flock 9
    if pgrep -f 'make.*O=.*/output' >/dev/null; then
        echo "A Buildroot build is running in output/, wait for it." >&2; exit 1
    fi
    {{fhs}} -- -c 'make phone-ui-rebuild' >output/.ui-build.log 2>&1 || {
        grep -E 'error|warning' output/.ui-build.log || tail -20 output/.ui-build.log; exit 1; }
    grep -E 'warning:' output/.ui-build.log | grep -v 'Git tree' || true
    info="$(git describe --always --dirty) $(date '+%F %T') $(whoami)"
    ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/phone-ui/dev && cd /var/lib/phone-ui/dev &&
        gunzip > phone-ui.new && chmod 755 phone-ui.new && mv phone-ui.new phone-ui &&
        sha256sum /usr/bin/phone-ui | cut -d' ' -f1 > base &&
        echo '$info' > info && echo '$*' > args && sync &&
        /etc/init.d/S90phone-ui restart && sleep 2 &&
        grep -v 'shader\|glsl\|GLSL\|^ *lv_opengl\|^$' /tmp/phone-ui.log" \
        < <(gzip -1 < output/build/phone-ui-1.0/phone-ui)

# Build the external app in apps/<name> (its Makefile, with the firmware's toolchain
# in CC, installs into OUT; BUILD and DL for scratch and downloads, STAGING for the
# headers plugin apps are built against) and install it in
# /var/lib/apps/<name>; restarts phone-ui
app name:
    #!/usr/bin/env bash
    set -euo pipefail
    out="$PWD/output/apps/{{name}}"
    rm -rf "$out"
    {{fhs}} -- -c 'make -C "$1" CC="$2" OUT="$3" BUILD="$4" DL="$5" STAGING="$6"' make "apps/{{name}}" \
        "$PWD/output/host/bin/aarch64-linux-gcc" "$out" "$PWD/output/apps-build/{{name}}" "$PWD/dl/apps" \
        "$PWD/output/staging"
    # Its icon.svg rendered at the menu's size (resvg from the dev shell)
    if [ -f "apps/{{name}}/icon.svg" ]; then
        resvg -w 112 -h 112 "apps/{{name}}/icon.svg" "$out/icon.png"
    fi
    # Unpacked next to it (hidden: not listed), then swapped
    tar -C "$out" -c . | gzip -1 | ssh {{ssh_opts}} {{phone}} "set -e; cd /var/lib && mkdir -p apps &&
        rm -rf apps/.{{name}}.new && mkdir apps/.{{name}}.new && gunzip | tar -xo -C apps/.{{name}}.new &&
        rm -rf apps/{{name}} && mv apps/.{{name}}.new apps/{{name}} && sync &&
        /etc/init.d/S90phone-ui restart && sleep 2 && { grep -E 'external app|plugin app|built for another|\.so' /tmp/phone-ui.log || true; }"

# Copy music (MP3 files, or directories of them) into the phone's /var/lib/music
music +paths:
    #!/usr/bin/env bash
    set -euo pipefail
    # One archive per path: busybox tar stops at the end of the first
    for p in "$@"; do
        p="${p%/}"
        tar -C "$(dirname "$p")" -c "$(basename "$p")" |
            ssh {{ssh_opts}} {{phone}} 'mkdir -p /var/lib/music && tar -xo -C /var/lib/music'
    done
    ssh {{ssh_opts}} {{phone}} 'sync && du -sh /var/lib/music'

# Copy ePub books into the phone's /var/lib/books (the Books app)
books +files:
    #!/usr/bin/env bash
    set -euo pipefail
    for f in "$@"; do
        dest=$(printf %q "/var/lib/books/$(basename "$f")")
        ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/books && cat > $dest" < "$f"
    done
    ssh {{ssh_opts}} {{phone}} 'sync && ls /var/lib/books'

# Copy game ROMs into the phone's /var/lib/genesis/roms (the Genesis app, apps/genesis);
# zip files are unpacked here (the core does not read them)
roms +files:
    #!/usr/bin/env bash
    set -euo pipefail
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    for f in "$@"; do
        case "${f,,}" in
            *.zip) unzip -q -o "$f" -d "$tmp" ;;
            *) cp "$f" "$tmp/" ;;
        esac
    done
    find "$tmp" -type f -print0 | while IFS= read -r -d '' rom; do
        dest=$(printf %q "/var/lib/genesis/roms/$(basename "$rom")")
        ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/genesis/roms && cat > $dest" < "$rom"
    done
    ssh {{ssh_opts}} {{phone}} 'sync && ls /var/lib/genesis/roms'

# Copy Doom IWADs (doom2.wad, freedoom1.wad...; zips unpacked) into the phone's
# /var/lib/doom/wads (the Doom app, apps/doom); names in lower case, as Doom knows them
wads +files:
    #!/usr/bin/env bash
    set -euo pipefail
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    for f in "$@"; do
        case "${f,,}" in
            *.zip) unzip -q -o "$f" -d "$tmp" ;;
            *) cp "$f" "$tmp/" ;;
        esac
    done
    find "$tmp" -type f -iname '*.wad' -print0 | while IFS= read -r -d '' wad; do
        name=$(basename "$wad")
        dest=$(printf %q "/var/lib/doom/wads/${name,,}")
        ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/doom/wads && cat > $dest" < "$wad"
    done
    ssh {{ssh_opts}} {{phone}} 'sync && ls /var/lib/doom/wads'

# BIOS files for apps/genesis (Sega CD), from files or zips of them: the ones the core
# expects are recognized by MD5 (the dumps it is known to work with) and installed under
# its names in /var/lib/genesis, the others ignored
bios +files:
    #!/usr/bin/env bash
    set -euo pipefail
    declare -A names=(
        [2efd74e3232ff260e371b99f84024f7f]=bios_CD_U.bin # Sega CD (U) Model 1 v1.10
        [e66fa1dc5820d254611fdcdba0662372]=bios_CD_E.bin # Mega-CD (E) Model 1 v1.00
        [278a9397d192149e84e820ac621a8edd]=bios_CD_J.bin # Mega-CD (J) Model 1 v1.00
    )
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT
    for f in "$@"; do
        case "${f,,}" in
            *.zip) unzip -q -o "$f" -d "$tmp" ;;
            *) cp "$f" "$tmp/" ;;
        esac
    done
    declare -A done=()
    while IFS= read -r -d '' f; do
        md5=$(md5sum "$f" | cut -d' ' -f1)
        name=${names[$md5]:-}
        [ -n "$name" ] && [ -z "${done[$name]:-}" ] || continue
        echo "$name: $(basename "$f")"
        ssh {{ssh_opts}} {{phone}} "mkdir -p /var/lib/genesis && cat > /var/lib/genesis/$name" < "$f"
        done[$name]=1
    done < <(find "$tmp" -type f -print0)
    [ ${#done[@]} -gt 0 ] || { echo "No known BIOS among them" >&2; exit 1; }
    ssh {{ssh_opts}} {{phone}} 'sync'

# Remove an external app from the phone; restarts phone-ui
app-rm name:
    ssh {{ssh_opts}} {{phone}} 'rm -rf /var/lib/apps/{{name}} && sync && /etc/init.d/S90phone-ui restart'

# Run phone-ui's tests on the host (phone-ui/tests), in the flake's `tests` shell (host
# LVGL and libraries, built the first time); `filter` runs only the tests whose name
# contains it
test filter="":
    nix develop .#tests -c make -C external/package/phone-ui/tests -j"$(nproc)" FILTER="{{filter}}"

# phone-ui in a desktop window, from the working tree (phone-ui/tests/sim.c): the mouse is the
# finger; End Power, Page Up/Down volume, Escape back, F5 an SMS, F6 the charger, F12 a
# screenshot, Ctrl+=/- zoom. `just sim --open Notes`, `--scale 0.75`
sim *args:
    nix develop .#tests -c make -C external/package/phone-ui/tests -j"$(nproc)" sim ARGS="{{args}}"

# Code coverage of the tests: per file here, line by line in
# phone-ui/tests/build/coverage/html/index.html, then the sources no test reaches
coverage:
    nix develop .#tests -c make -C external/package/phone-ui/tests -j"$(nproc)" coverage

# Translation coverage of phone-ui: per file, the texts marked for translation (i18n.h) or not.
# `just i18n --list calendar.c` lists those not marked (file:line), NON-NLS ones left out
i18n *args:
    nix develop .#tests -c python3 external/package/phone-ui/i18n-check.py {{args}}

# Preview the user documentation (docs/) at http://localhost:8000, rebuilt on changes;
# `just docs 0.0.0.0:8000` to reach it from another machine
docs addr="localhost:8000": docs-shots docs-demo
    zensical serve --dev-addr {{addr}}

# Build the user documentation into site/; a broken link between pages fails it, and an
# image or a Font Awesome icon a page shows that is not there (Zensical does not check them)
docs-build: docs-shots docs-demo
    #!/usr/bin/env bash
    set -euo pipefail
    missing=0
    while IFS=: read -r page img; do
        [ -f "$(dirname "$page")/$img" ] || { echo "$page: no $img" >&2; missing=1; }
    done < <(grep -roP '!\[[^]]*\]\(\K[^)# ]+' docs --include='*.md' | grep -v '://' || true)
    # Font Awesome shortcodes (:fontawesome-solid-trash-can:) left as text when unknown
    icons=("$(dirname "$(readlink -f "$(command -v zensical)")")"/../lib/python3*/site-packages/zensical/templates/.icons)
    while IFS=: read -r page code; do
        f=${code#:fontawesome-}; f=${f%:}; style=${f%%-*}
        [ -f "${icons[0]}/fontawesome/$style/${f#*-}.svg" ] || { echo "$page: no icon $code" >&2; missing=1; }
    done < <(grep -roE ':fontawesome-(solid|regular|brands)-[a-z0-9-]+:' docs --include='*.md' || true)
    [ "$missing" = 0 ]
    zensical build --clean --strict

# phone-ui in the browser (phone-ui/wasm): WebAssembly in phone-ui/wasm/build/, with a page
# that shows it
wasm:
    nix develop .#wasm -c make -C external/package/phone-ui/wasm -j"$(nproc)"

# The browser build at http://localhost:8080
wasm-serve: wasm
    nix develop .#wasm -c make -C external/package/phone-ui/wasm serve

# The browser build's smoke test: phone-ui used in headless Chromium (wasm/smoke.mjs)
wasm-smoke: wasm
    nix develop .#wasm -c make -C external/package/phone-ui/wasm smoke

# The browser build for the documentation's front page, into docs/demo/ (ignored); the emoji
# only when their set changed: replacing their 1906 files under `just docs` broke its rebuild;
# the demo files (demo/) only when newer
docs-demo: wasm
    mkdir -p docs/demo
    cd external/package/phone-ui/wasm/build && cp index.html phone-ui.js phone-ui.wasm {{justfile_directory()}}/docs/demo/
    cp -rLu --no-preserve=mode external/package/phone-ui/wasm/build/demo docs/demo/
    src=$(readlink -f external/package/phone-ui/wasm/build/emoji); stamp=external/package/phone-ui/wasm/build/emoji-copied; \
    if [ ! -d docs/demo/emoji ] || [ "$(cat $stamp 2>/dev/null)" != "$src" ]; then \
        rm -rf docs/demo/emoji && cp -rL --no-preserve=mode "$src" docs/demo/emoji && echo "$src" > $stamp; fi

# The documentation's screenshots, made by phone-ui's tests on the host (tests/test_docs.c)
# into docs/images/generated/; `filter` makes only the images of the tests whose name has it
docs-shots filter="":
    nix develop .#tests -c make -C external/package/phone-ui/tests -j"$(nproc)" docs-shots FILTER="{{filter}}"

# Drop the development build pushed by `just ui`, run the installed phone-ui
ui-reset:
    ssh {{ssh_opts}} {{phone}} 'rm -rf /var/lib/phone-ui/dev && /etc/init.d/S90phone-ui restart'

# iptables, not nft: Docker's FORWARD policy is DROP, the rules go before it.
# Lasts until `just unshare-internet` or the host reboots.
# Share the host's internet connection with the phone over USB (asks for sudo)
share-internet:
    #!/usr/bin/env bash
    set -euo pipefail
    iface=$(ip -o link | awk -F': ' '/02:50:48:00:00:02/ { print $2 }')
    [ -n "$iface" ] || { echo "The phone's USB network is not up." >&2; exit 1; }
    sudo sysctl -q -w net.ipv4.ip_forward=1
    rule() { sudo iptables -t "$1" -C "${@:2}" 2>/dev/null || sudo iptables -t "$1" -I "${@:2}"; }
    rule nat POSTROUTING -s 172.16.42.0/24 ! -o "$iface" -j MASQUERADE
    rule filter FORWARD -i "$iface" -j ACCEPT
    rule filter FORWARD -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT
    echo "Sharing the internet connection with the phone ($iface)."

# Stop sharing the internet connection (IP forwarding stays on)
unshare-internet:
    #!/usr/bin/env bash
    set -euo pipefail
    iface=$(ip -o link | awk -F': ' '/02:50:48:00:00:02/ { print $2 }')
    [ -n "$iface" ] || { echo "The phone's USB network is not up." >&2; exit 1; }
    sudo iptables -t nat -D POSTROUTING -s 172.16.42.0/24 ! -o "$iface" -j MASQUERADE || true
    sudo iptables -t filter -D FORWARD -i "$iface" -j ACCEPT || true
    sudo iptables -t filter -D FORWARD -o "$iface" -m conntrack --ctstate RELATED,ESTABLISHED -j ACCEPT || true

# Follow phone-ui's output (quit: Ctrl-c)
ui-log:
    ssh {{ssh_opts}} {{phone}} tail -n 50 -f /tmp/phone-ui.log

# Screenshot of the phone's display into screenshots/ (or `file`), with the pushed
# phone-ui (`just ui`) or the installed one, whichever knows --screenshot
screenshot file="":
    #!/usr/bin/env bash
    set -euo pipefail
    out="${1:-screenshots/$(date +%Y%m%d_%H%M%S).jpg}"
    mkdir -p "$(dirname "$out")"
    ssh {{ssh_opts}} {{phone}} 'for b in /var/lib/phone-ui/dev/phone-ui /usr/bin/phone-ui; do
        [ -x $b ] && grep -q -- --screenshot $b && exec $b --screenshot -; done
        echo "No phone-ui with --screenshot on the phone: just ui first" >&2; exit 1' > "$out.part" ||
        { rm -f "$out.part"; exit 1; }
    mv "$out.part" "$out"
    echo "$out"

# SSH into the phone over the USB network
ssh *args:
    ssh {{ssh_opts}} {{phone}} "$@"

# Serial console over the USB gadget, reconnects across reboots (quit: Ctrl-t q)
console dev="/dev/ttyACM0":
    tio {{dev}}

# Follow the kernel log over SSH, from the start of each boot, across reboots (quit: Ctrl-c)
logs:
    #!/usr/bin/env bash
    while true; do
        # /dev/kmsg lines are "prio,seq,usec,flags;message", continuation lines start with a space
        ssh {{ssh_opts}} -o ConnectTimeout=2 -o ServerAliveInterval=2 {{phone}} cat /dev/kmsg 2>/dev/null \
            | grep --line-buffered -v '^ ' | sed -u 's/^[^;]*;//'
        echo "--- phone not reachable, waiting ---"
        until ssh {{ssh_opts}} -o ConnectTimeout=2 -o BatchMode=yes {{phone}} true 2>/dev/null; do sleep 1; done
    done
