{
  description = "Paravision development shell";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-25.05";
  };

  outputs = { self, nixpkgs }:
    let
      supportedSystems = [
        "x86_64-linux"
        "aarch64-linux"
        "x86_64-darwin"
        "aarch64-darwin"
        "i686-linux"
      ];
      forAllSystems = nixpkgs.lib.genAttrs supportedSystems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs { inherit system; };
          isLinux = pkgs.stdenv.isLinux;
          isDarwin = pkgs.stdenv.isDarwin;
        in
        {
          default = pkgs.mkShell.override {
            stdenv =
              if isLinux
              then pkgs.overrideCC pkgs.gcc14Stdenv pkgs.llvmPackages_20.clang
              else pkgs.llvmPackages_20.stdenv;
          } {
            name = "pollcoro-shell";

            buildInputs = with pkgs; [
              git
              bash
              cmake
              ninja
              pkg-config
              cmake-format
              llvmPackages_20.clang-tools
              just
            ] ++ pkgs.lib.optionals isLinux [
              gdb
              valgrind
            ] ++ pkgs.lib.optionals isDarwin [
              lldb
            ];

            shellHook = ''
              set -eo pipefail

              export PATH="${pkgs.llvmPackages_20.clang-tools}/bin:$PATH"
            '';
          };
        });
    };
}
