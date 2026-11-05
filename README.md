# PinePhone feature phone firmware

From-scratch firmware for the original PinePhone: Buildroot, mainline Linux and an LVGL
feature phone UI. The host needs Nix with flakes; all tooling comes from the flake.

```sh
nix develop
just build    # the image into output/images/
just deploy   # build, then flash
```

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
