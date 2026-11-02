{
  description = "From-scratch feature phone firmware for the PinePhone (A64)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      system = "x86_64-linux";
      pkgs = nixpkgs.legacyPackages.${system};
      cross = pkgs.pkgsCross.aarch64-multiplatform;
    in
    {
      packages.${system} = {
        # U-Boot that is only ever loaded into RAM over FEL. Its sole job is to
        # expose the eMMC to the host as a USB mass storage device.
        uboot-flasher = cross.buildUBoot {
          defconfig = "pinephone_defconfig";
          extraMeta.platforms = [ "aarch64-linux" ];
          env = {
            BL31 = "${cross.armTrustedFirmwareAllwinner}/bl31.bin";
            SCP = "/dev/null";
          };
          extraConfig = ''
            CONFIG_USB_MUSB_GADGET=y
            CONFIG_USB_GADGET=y
            CONFIG_USB_GADGET_DOWNLOAD=y
            CONFIG_CMD_USB_MASS_STORAGE=y
            CONFIG_BOOTDELAY=0
          '';
          filesToInstall = [ "u-boot-sunxi-with-spl.bin" ];
        };

        # Script run by U-Boot when FEL-booted (see bootcmd_fel in sunxi-common.h).
        # mmc 1 is the eMMC (alias mmc1 = &mmc2 in the PinePhone device tree).
        fel-ums-script = pkgs.runCommand "fel-ums.scr" { nativeBuildInputs = [ pkgs.ubootTools ]; } ''
          cat > ums.cmd <<'EOF'
          echo "Exporting eMMC over USB mass storage"
          ums 0 mmc 1
          EOF
          mkdir -p $out
          mkimage -A arm64 -T script -C none -n "eMMC UMS" -d ums.cmd $out/fel-ums.scr
        '';

        flasher = pkgs.writeShellApplication {
          name = "pinephone-flash";
          runtimeInputs = with pkgs; [ sunxi-tools usbutils util-linux coreutils systemd ];
          text = ''
            export UBOOT=${self.packages.${system}.uboot-flasher}/u-boot-sunxi-with-spl.bin
            export SCRIPT=${self.packages.${system}.fel-ums-script}/fel-ums.scr
            ${builtins.readFile ./scripts/pinephone-flash.sh}
          '';
        };

        default = self.packages.${system}.flasher;
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [ sunxi-tools ubootTools usbutils ];
      };
    };
}
