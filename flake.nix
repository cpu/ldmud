{
  description = "LDMud - Gamedriver for LPMuds";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };

          # Base ldmud with local source. Fortify is disabled across all
          # variants because access_check.c uses a pre-C99 struct-hack for
          # its access_class message buffer that fortify (rightly) flags
          # as a buffer overflow on every login. See allow_host_access_crash.txt
          # for the proper upstream fix.
          ldmud = pkgs.ldmud.overrideAttrs (oldAttrs: {
            version = "dev";
            src = self;
            sourceRoot = "source/src";
            patches = [ ./mysql-compat.patch ];
            hardeningDisable = (oldAttrs.hardeningDisable or [ ]) ++ [ "fortify" "fortify3" ];
          });

          # Full-featured ldmud with all optional features enabled
          ldmud-full = ldmud.override {
            ipv6Support = true;
            mccpSupport = true;
            mysqlSupport = true;
            postgresSupport = true;
            sqliteSupport = true;
            tlsSupport = true;
            pythonSupport = true;
          };

          # Enable LPC profiler for flame graph generation
          withProfiler =
            drv:
            drv.overrideAttrs (oldAttrs: {
              configureFlags = (oldAttrs.configureFlags or [ ]) ++ [ "--enable-use-lpc-profiler" ];
            });

          # Debug build with symbols and no optimization (includes profiler)
          withDebug =
            drv:
            drv.overrideAttrs (oldAttrs: {
              pname = "${oldAttrs.pname}-debug";
              configureFlags = (oldAttrs.configureFlags or [ ]) ++ [ "--enable-use-lpc-profiler" ];
              env = (oldAttrs.env or { }) // {
                NIX_CFLAGS_COMPILE = "${oldAttrs.env.NIX_CFLAGS_COMPILE or ""} -g3 -O0 -fno-omit-frame-pointer";
              };
              dontStrip = true;
              hardeningDisable = [ "all" ];
            });

          ldmud-profiler = withProfiler ldmud;
          ldmud-debug = withDebug ldmud;

          # ASAN-enabled builds
          withAsan =
            drv:
            drv.overrideAttrs (oldAttrs: {
              pname = "${oldAttrs.pname}-asan";
              env = (oldAttrs.env or { }) // {
                NIX_CFLAGS_COMPILE = "${
                  oldAttrs.env.NIX_CFLAGS_COMPILE or ""
                } -fsanitize=address -fno-omit-frame-pointer -O1";
                NIX_CFLAGS_LINK = "-fsanitize=address";
                # Disable leak detection during build (build tools have benign leaks)
                ASAN_OPTIONS = "detect_leaks=0";
              };
              # Disable hardening that can interfere with ASAN
              hardeningDisable = [ "all" ];
            });

          ldmud-asan = withAsan ldmud;
          ldmud-full-asan = withAsan ldmud-full;
        in
        {
          inherit
            ldmud
            ldmud-full
            ldmud-profiler
            ldmud-debug
            ldmud-asan
            ldmud-full-asan
            ;
          default = ldmud;
        }
      );

      devShells = forAllSystems (
        system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };

          # Wrapper: turn a collapsed-stack file into an SVG flamegraph.
          #   lpc-flamegraph <input.collapsed> [output.svg]
          # If output is omitted, writes <input>.svg next to the input.
          lpc-flamegraph = pkgs.writeShellApplication {
            name = "lpc-flamegraph";
            runtimeInputs = [ pkgs.flamegraph ];
            text = ''
              if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
                echo "usage: lpc-flamegraph <input.collapsed> [output.svg]" >&2
                exit 1
              fi
              in=$1
              out=''${2:-''${in%.collapsed}.svg}
              if [ "$out" = "$in" ]; then
                out="$in.svg"
              fi
              flamegraph.pl --title "LPC Flame Graph" --countname samples "$in" > "$out"
              echo "wrote $out"
            '';
          };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.ldmud ];
            packages = [
              pkgs.flamegraph
              lpc-flamegraph
            ];
          };
        }
      );
    };
}
