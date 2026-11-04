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
          runtimeInputs = with pkgs; [ sunxi-tools usbutils util-linux coreutils systemd openssh pv gzip ];
          text = ''
            export UBOOT=${self.packages.${system}.uboot-flasher}/u-boot-sunxi-with-spl.bin
            export SCRIPT=${self.packages.${system}.fel-ums-script}/fel-ums.scr
            export PP_UPDATE=${./external/board/pinephone/rootfs-overlay/usr/sbin/pp-update}
            ${builtins.readFile ./scripts/pinephone-flash.sh}
          '';
        };

        # Buildroot expects a conventional FHS Linux host: `nix run .#fhs`, then `make`.
        fhs = pkgs.buildFHSEnv {
          name = "buildroot-fhs";
          targetPkgs = p: with p; [
            gcc gnumake binutils pkg-config ncurses ncurses.dev
            bash coreutils findutils diffutils gnused gnugrep gawk which file patch
            gzip bzip2 xz zstd lzop gnutar cpio unzip
            perl python3 bc rsync wget git util-linux openssh
            openssl openssl.dev zlib zlib.dev
          ];
          profile = ''
            unset LD_LIBRARY_PATH
            export PS1="(buildroot) $PS1"
          '';
        };

        # LVGL for the host, for phone-ui's tests (`just test`): the image's version, patches
        # and lv_conf.h, without the NEON blending (x86); the tests create no display driver,
        # nothing loads GL
        lvgl-host = pkgs.stdenv.mkDerivation {
          pname = "lvgl-host";
          version = "9.6.0";
          src = pkgs.fetchFromGitHub {
            owner = "lvgl";
            repo = "lvgl";
            rev = "v9.6.0";
            hash = "sha256-CF+Vd0f+oONispeiPZpnnLwlBm/EQsJWa177SgNB9Oo=";
          };
          patches = map (p: ./external/patches/lvgl + "/${p}")
            (builtins.attrNames (builtins.readDir ./external/patches/lvgl));
          buildInputs = with pkgs; [ libdrm libgbm libGL ];
          makeFlags = [ "-f" "${./external/package/lvgl/Makefile.lib}" "LV_CONF=lv_conf.h"
            "CFLAGS=-I${pkgs.libdrm.dev}/include/libdrm" ];
          preBuild = ''
            sed 's/^#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON$/#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE/' \
              ${./external/package/lvgl/lv_conf.h} > lv_conf.h
            grep -q '^#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE$' lv_conf.h
          '';
          installFlags = [ "DESTDIR=$(out)" ];
        };

        default = self.packages.${system}.flasher;
      };

      devShells.${system}.default = pkgs.mkShell {
        packages = with pkgs; [ just sunxi-tools ubootTools usbutils openssh tio lv_font_conv resvg imagemagick unzip jq gcovr ];
        # gen-fonts.sh: static Inter Medium (text) and Inter Display Light (the home screen's
        # clock), out of the collection nixpkgs ships: lv_font_conv reads neither .ttc nor
        # variable fonts.
        INTER = pkgs.runCommand "inter-static" { nativeBuildInputs = [ pkgs.python3Packages.fonttools ]; } ''
          mkdir $out
          python3 - <<'EOF'
          from fontTools.ttLib import TTCollection
          import os
          out = os.environ["out"]
          for font in TTCollection("${pkgs.inter}/share/fonts/truetype/Inter.ttc").fonts:
              name = font["name"].getDebugName(4)
              if name in ("Inter Medium", "Inter Display Light"):
                  font.save(f"{out}/{name.replace(' ', '-')}.ttf")
          EOF
          test -f $out/Inter-Medium.ttf && test -f $out/Inter-Display-Light.ttf
        '';
      };
    };
}
