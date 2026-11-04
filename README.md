# PinePhone feature phone firmware

From-scratch firmware for the original PinePhone: Buildroot, mainline Linux and an LVGL
feature phone UI. The host needs Nix with flakes; all tooling comes from the flake.

```sh
nix develop
just build    # the image into output/images/
just deploy   # build, then flash
```

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
