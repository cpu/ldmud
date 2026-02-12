{ pkgs ? import <nixpkgs> { } }:
pkgs.mkShell {
  nativeBuildInputs = [
    #pkgs.jetbrains.clion
    pkgs.pkg-config
    pkgs.autoreconfHook
    pkgs.automake
    pkgs.bison
    pkgs.libiconv
    pkgs.pcre
    pkgs.libgcrypt
    pkgs.libxcrypt
    pkgs.openssl
    pkgs.libmysqlclient
  ];
}
