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
            # libc's headers in /usr/include: gcc finds them without, but not CMake's
            # find_path (host-cmake's libarchive looks for iconv.h)
            glibc.dev
          ];
          profile = ''
            unset LD_LIBRARY_PATH
            export PS1="(buildroot) $PS1"
          '';
        };

        # LVGL for the host, for phone-ui's tests (`just test`): the image's version, patches
        # and lv_conf.h, without the NEON blending (x86), with LVGL's test module (emulated
        # input and time)
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
            sed -e 's/^#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NEON$/#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE/' \
              -e 's/^#define LV_USE_TEST 0$/#define LV_USE_TEST 1/' \
              ${./external/package/lvgl/lv_conf.h} > lv_conf.h
            grep -q '^#define LV_USE_DRAW_SW_ASM LV_DRAW_SW_ASM_NONE$' lv_conf.h
            grep -q '^#define LV_USE_TEST 1$' lv_conf.h
          '';
          installFlags = [ "DESTDIR=$(out)" ];
        };

        # lvgl-host's sources with the image's patches, for the WebAssembly build (`just wasm`)
        lvgl-src = pkgs.applyPatches {
          name = "lvgl-src-9.6.0";
          inherit (self.packages.${system}.lvgl-host) src patches;
        };

        # litehtml as in the image (external/package/litehtml, nixpkgs has 0.9), with its gumbo
        litehtml-host = pkgs.stdenv.mkDerivation {
          pname = "litehtml-host";
          version = "0.10";
          src = pkgs.fetchFromGitHub {
            owner = "litehtml";
            repo = "litehtml";
            rev = "v0.10";
            hash = "sha256-OPyluMC7dSVvS4/tFNzeogOT6elA7paW3duTR+8/VuE=";
          };
          nativeBuildInputs = [ pkgs.cmake ];
          cmakeFlags = [ "-DBUILD_SHARED_LIBS=OFF" "-DLITEHTML_BUILD_TESTING=OFF" ];
        };

        # The emoji images as external/package/noto-emoji installs them in /usr/share/emoji:
        # phone-ui's tests draw emoji with them (EMOJI_DIR)
        emoji-host = pkgs.stdenv.mkDerivation {
          pname = "noto-emoji";
          version = "16.0.0";
          src = pkgs.fetchurl {
            url = "https://registry.npmjs.org/emoji-datasource-google/-/emoji-datasource-google-16.0.0.tgz";
            sha256 = "fb5fd131b362aab7a55542d2a147a79708cce0a8b1c1bbacd470172708b5c438";
          };
          installPhase = ''
            mkdir -p $out
            for f in img/google/64/*.png; do
              name=''${f##*/}; name=''${name%.png}
              case $name in *1f3f[b-f]*) continue ;; esac
              cp $f $out/''${name//-fe0f/}.png
            done
          '';
        };

        default = self.packages.${system}.flasher;
      };

      devShells.${system} = {
        default = pkgs.mkShell {
          packages = with pkgs; [ just sunxi-tools ubootTools usbutils openssh tio lv_font_conv resvg imagemagick unzip jq
            # gen-fonts.sh: ttx, the Font Awesome glyphs' names
            python3Packages.fonttools
            # the user documentation (docs/user, zensical.toml): just docs
            zensical ];
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
          # gen-fonts.sh: Font Awesome 7 Free Solid, for the icons of phone-ui's symbols.txt
          FONTAWESOME = "${pkgs.font-awesome}/share/fonts/opentype";
          # gen-fonts.sh: Literata (serif, made for reading on screens), Books' text
          LITERATA = "${pkgs.literata}/share/fonts/truetype";
        };

        # phone-ui's tests (`just test`, `just coverage`) and simulator (`just sim`): its libraries for the host, Mesa
        # for a GL context without a GPU (llvmpipe), bubblewrap for the sandbox they run in,
        # cwebp for the documentation's screenshots (`just docs-shots`)
        tests = pkgs.mkShell {
          packages = with pkgs; [ gnumake pkg-config gcovr jq bubblewrap libwebp resvg ];
          buildInputs = with pkgs; [
            curl libpng libjpeg libdrm mpg123 alsa-lib libgbm libGL zlib self.packages.${system}.litehtml-host
            # Browser's test (test_web.c): GLib, libwpe's and WPEBackend-fdo's headers (the image's versions)
            glib libwpe libwpe-fdo
            # the simulator's window (`just sim`)
            sdl3
          ];
          LVGL = self.packages.${system}.lvgl-host;
          EGL_VENDOR = "${pkgs.mesa}/share/glvnd/egl_vendor.d/50_mesa.json";
          EMOJI = self.packages.${system}.emoji-host;
        };

        # phone-ui in the browser (`just wasm`): Emscripten, LVGL, litehtml, libjpeg-turbo's and
        # libmpg123's sources (Emscripten's libjpeg port lacks its BGRA output, it has no mpg123),
        # the headers of the libraries it replaces (ALSA, libcurl: wasm/ has them), the emoji
        # images; Node and Chromium for its smoke test
        wasm = pkgs.mkShell {
          packages = with pkgs; [ emscripten gnumake cmake python3 nodejs chromium ];
          LVGL_SRC = self.packages.${system}.lvgl-src;
          LITEHTML_SRC = self.packages.${system}.litehtml-host.src;
          LIBJPEG_SRC = pkgs.libjpeg_turbo.src;
          LIBMPG123_SRC = pkgs.srcOnly pkgs.mpg123;
          EMOJI = self.packages.${system}.emoji-host;
          LIB_HEADERS = pkgs.symlinkJoin {
            name = "phone-ui-wasm-headers";
            paths = with pkgs; [ alsa-lib.dev curl.dev ];
          };
        };
      };
    };
}
