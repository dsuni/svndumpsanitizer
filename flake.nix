{
  inputs = {
    nixpkgs.url = github:NixOS/nixpkgs/nixos-23.11;
    flake-parts.url = github:hercules-ci/flake-parts;
    flake-root.url = github:srid/flake-root;
    pre-commit-hooks = {
      url = github:cachix/pre-commit-hooks.nix;
      inputs = {
        nixpkgs.follows = "nixpkgs";
      };
    };
  };

  outputs = inputs:
    inputs.flake-parts.lib.mkFlake {inherit inputs;} {
      imports = with inputs; [
        flake-root.flakeModule
        pre-commit-hooks.flakeModule
      ];
      systems = ["x86_64-linux" "aarch64-linux" "aarch64-darwin" "x86_64-darwin"];
      perSystem = {
        config,
        self',
        inputs',
        pkgs,
        system,
        ...
      }: let
        svndumpsanitizer = pkgs.stdenv.mkDerivation {
          pname = "svndumpsanitizer";
          version = "v0.6.4";
          src = ./.;
          buildPhase = ''
            gcc svndumpsanitizer.c -o svndumpsanitizer
          '';
          installPhase = ''
            mkdir -p $out/bin
            cp svndumpsanitizer $out/bin
          '';
          nativeBuildInputs = with pkgs; [gcc];
          meta = with pkgs.lib; {
            description = "A program aspiring to be a more advanced version of svndumpfilter";
            homepage = "https://github.com/dsuni/svndumpsanitizer";
            license = licenses.gpl3;
            maintainers = with maintainers; [lafrenierejm];
            mainProgram = "svndumpsanitizer";
          };
        };
      in {
        # Pre-commit hooks.
        pre-commit = {
          check.enable = true;
          settings.hooks = {
            alejandra.enable = true;
          };
        };

        # `nix build`
        packages = {
          inherit svndumpsanitizer;
          default = svndumpsanitizer;
        };

        # `nix run`
        apps = let
          svndumpsanitizer = {
            type = "app";
            program = "${self'.packages.svndumpsanitizer}/bin/svndumpsanitizer";
          };
        in {
          inherit svndumpsanitizer;
          default = svndumpsanitizer;
        };

        # `nix develop`
        devShells.default = pkgs.mkShell {
          inputsFrom = [config.pre-commit.devShell svndumpsanitizer];
          packages = with pkgs; [rnix-lsp clang-tools];
        };
      };
    };
}
