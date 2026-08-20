{
  description = "anoa — a browser you drive from the command line";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      # qtwebengine in nixpkgs is not built for x86_64-darwin, so an Intel Mac
      # cannot build this at all — Homebrew has a universal build for those.
      systems = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
      forEach = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Keep this in step with CMakeLists.txt. It is passed to the build rather
      # than read from it, so `nix run` reports the same string a release does.
      version = "0.12.0";
    in
    {
      packages = forEach (pkgs: rec {
        anoa = pkgs.callPackage ./nix/anoa.nix { inherit version; };
        default = anoa;
      });

      # `nix run github:porcupine-md/anoa-browser -- --headless --port 9222`
      apps = forEach (pkgs:
        let anoa = self.packages.${pkgs.system}.anoa; in rec {
          anoa-app = {
            type = "app";
            program = "${anoa}/bin/anoa";
          };
          default = anoa-app;
        });

      # Everything the build needs, plus what the test suites want: node for the
      # vitest and Playwright suites, python for the shell harnesses.
      devShells = forEach (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.anoa ];
          packages = with pkgs; [ nodejs python3 ];
        };
      });

      formatter = forEach (pkgs: pkgs.nixpkgs-fmt);
    };
}
