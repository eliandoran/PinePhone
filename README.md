# PinePhone feature phone

A complete operating system that turns the original PinePhone into a simple, fast feature
phone. It starts in a few seconds, opens its apps at once, and does the everyday things a phone
is for: calls, text messages, contacts, notes, music, a camera.

<p align="center">
  <img src="https://eliandoran.github.io/PinePhone/images/generated/home.webp" width="200" alt="The home screen">
  <img src="https://eliandoran.github.io/PinePhone/images/generated/messages-thread.webp" width="200" alt="A conversation in Messages">
  <img src="https://eliandoran.github.io/PinePhone/images/generated/maps-town.webp" width="200" alt="Maps">
  <img src="https://eliandoran.github.io/PinePhone/images/generated/music-now-playing.webp" width="200" alt="Music playing a song">
</p>

**[Try it in your browser](https://eliandoran.github.io/PinePhone/)** ·
**[Install it](https://eliandoran.github.io/PinePhone/install/)** ·
**[Documentation](https://eliandoran.github.io/PinePhone/)**

The browser demo is the phone's own interface compiled to WebAssembly, with pretend hardware.
No PinePhone needed.

## What it does

- **Phone**: calls, text messages, operator codes (USSD) and mobile data, up to 4G. An
  incoming call or message wakes the phone from sleep.
- **Everyday apps**: Contacts, Clock (alarm, timer, stopwatch, world clock), Calendar,
  Calculator, Notes, Tasks, Recorder, Files and Compass.
- **Media**: Camera, Gallery, Music and Books (ePub).
- **Internet** over Wi-Fi or mobile data: Maps, Weather and Web Lite, a browser for simple
  pages.
- **Bluetooth**: speakers, headsets for calls, and game controllers.
- **Apps installed separately**: Telegram, Email, GitHub, an AI assistant, Terminal, a full
  browser (WPE WebKit), Doom and a Sega Genesis emulator.
- **With a computer**: file transfer over USB (MTP), and updates sent over the cable to a
  second slot, undone automatically when the new version does not start.
- An on-screen keyboard with autocorrection, in English and Romanian.

## How it is built

It is built like a feature phone's firmware: one image, flashed and updated as a whole, with no
package manager. It replaces the phone's operating system (postmarketOS, Mobian...) rather than
adding to it, so none of it installs under another system. What others can take are its kernel
patches, in `external/patches/linux/`. It is an embedded system, not a small desktop:

- The whole system is built from source with [Buildroot](https://buildroot.org/) on a
  mainline Linux kernel, with BusyBox as init. The root filesystem is read-only.
- The interface is one program written in C on [LVGL](https://lvgl.io/). It draws straight
  to the display through DRM/KMS, with the GPU (Mesa's Lima driver). There is no Wayland and
  no compositor.
- The modem is driven with AT commands, and sound goes through ALSA.
- Most of the interface runs at the display's refresh rate.

## Before you try it

- It runs on the **original PinePhone** only (Allwinner A64), not the PinePhone Pro.
- **Calls go over 2G or 3G.** Calls over 4G (VoLTE) are turned off, so calls do not work in
  countries where those networks are gone. Text messages and data work on 4G.
- The mobile network has been tried on one operator (Digi, Romania).
- The separately installed apps are installed from a computer that built the firmware. There
  is no app store.
- This is a personal project, shared as it is.

The firmware can be tried from a microSD card, which leaves the phone's own system as it is.
From a card, the phone starts a little slower and large apps take longer to open the first
time: for the full speed, install it on the phone's storage, which erases what is there. Both
are in
[Installing](https://eliandoran.github.io/PinePhone/install/).

## Building

The host needs [Nix](https://nixos.org/download/) with flakes: all the tooling comes from the
flake, and Buildroot runs inside an FHS environment it provides.

```sh
nix develop
just build    # the image into output/images/
just deploy   # build, then flash a phone that runs the firmware
just sim      # the interface in a desktop window, without a phone
just test     # its tests, on the host
```

The first build takes several hours, mostly WebKit for the separately installed browser.
`just` alone lists the other commands. [CLAUDE.md](CLAUDE.md) describes the layout, the boot
and update design and the hardware notes; it is written for the AI assistant used on this
project, and is the most complete developer document.

## AI

This project was developed with heavy use of AI (Claude Code), each change tried on the phone
and reviewed. The interface has a test suite that runs it headless on every push, against
fakes of the modem, Wi-Fi, Bluetooth, cameras and sound.

## License

The firmware's own code (the interface, the apps in `apps/`, the build and board files, the
scripts) is under the GNU General Public License, version 2 or, at your option, any later
version: see [LICENSE](LICENSE).

Not everything here is ours:

- A patch in `external/patches/` or `external/buildroot-patches/` is under the license of the
  project it changes, so that it can go upstream.
- Code and data taken from other projects keep their own licenses, stated beside them: the
  word lists in `external/package/phone-ui/words/`, the files in `demo/`, the fonts and icons.
- The build downloads other software (the kernel, LVGL, WebKit, the games' engines...), each
  under its own license. The Genesis app's engine, Genesis Plus GX, allows no commercial use.

## Cleaning patched packages

Buildroot applies the patches in `external/patches/<pkg>/` (`linux`, `lvgl`, `wpewebkit`)
only when it extracts a package's source. After pulling or adding a patch, an ordinary build
keeps the old tree and leaves the new patches out, even when it recompiles the package.
Start the package over from a fresh source tree with `<pkg>-dirclean`.

To see which packages are stale, compare each patch directory with
`output/build/<pkg>-*/.applied_patches_list`, which lists the patches applied at extraction.

### Linux kernel

```sh
just make linux-dirclean   # delete output/build/linux-*/
just build                 # extract, patch, configure and build it again
just flash
```

On the phone, `/proc/config.gz` must then have what the patches add (e.g.
`CONFIG_TYPEC_ANX7688`).

### LVGL

LVGL is linked statically into phone-ui, so phone-ui has to be rebuilt after it:

```sh
just make lvgl-dirclean
just make lvgl phone-ui-rebuild all
just flash                 # or `just ui` to try it without flashing
```

Plugin apps (e.g. `apps/telegram`) build against LVGL's headers: if the patches change them,
run `just app <name>` again, or phone-ui refuses to load the plugin.

### Without cleaning

Not every kernel change needs a clean:

- `linux.fragment` or `linux.defconfig`: `just make linux-reconfigure all`
- kernel source edited in place: `just make linux-rebuild all`

## Building on Windows (NixOS-WSL)

The sources can stay on the Windows drive (e.g. edited with VS Code on Windows) while the
build runs in [NixOS-WSL](https://github.com/nix-community/NixOS-WSL).

1. Enable flakes and the `nix` command. In `/etc/nixos/configuration.nix`, add:

   ```nix
   nix.settings.experimental-features = [ "nix-command" "flakes" ];
   ```

   then apply it with `sudo nixos-rebuild switch`. (For your user only, without a rebuild:
   `echo 'experimental-features = nix-command flakes' >> ~/.config/nix/nix.conf`.)

2. Keep the build trees on WSL's own filesystem. `/mnt/c` is several times slower and has
   no Linux permissions, which breaks Buildroot (and SSH refuses the phone's key there).
   From the repository in WSL:

   ```sh
   cd /mnt/c/Users/<you>/Projects/PinePhone
   nix develop
   just build-dir ~/pinephone-build
   ```

   `buildroot/`, `output/`, `dl/` and `keys/` become links to `~/pinephone-build`. From
   Windows, the build output is at `\\wsl$\NixOS\home\nixos\pinephone-build`.

3. Build as usual: `just build`.

Line endings are fixed to LF by `.gitattributes`, whatever Git for Windows' `core.autocrlf`
says, and the Makefile drops the Windows entries (with spaces) that WSL adds to `PATH`.
