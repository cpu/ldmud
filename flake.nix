{
  description = "LDMud - Gamedriver for LPMuds";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [ "x86_64-linux" "aarch64-linux" "x86_64-darwin" "aarch64-darwin" ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };

          # Base ldmud with local source
          ldmud = pkgs.ldmud.overrideAttrs (oldAttrs: {
            version = "dev";
            src = self;
            sourceRoot = "source/src";
            patches = [ ./mysql-compat.patch ];
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

          # ASAN-enabled builds
          withAsan = drv: drv.overrideAttrs (oldAttrs: {
            pname = "${oldAttrs.pname}-asan";
            env = (oldAttrs.env or {}) // {
              NIX_CFLAGS_COMPILE = "${oldAttrs.env.NIX_CFLAGS_COMPILE or ""} -fsanitize=address -fno-omit-frame-pointer -O1";
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
          inherit ldmud ldmud-full ldmud-asan ldmud-full-asan;
          default = ldmud;
        });

      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
            config.allowUnfree = true;
          };
        in
        {
          default = pkgs.mkShell {
            inputsFrom = [ self.packages.${system}.ldmud ];
          };
        });
    };
}
