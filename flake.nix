{
  description = "A very light linux rendering backend library";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    systems.url = "github:nix-systems/default-linux";

    hyprutils = {
      url = "github:hyprwm/hyprutils";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
    };

    hyprwayland-scanner = {
      url = "github:hyprwm/hyprwayland-scanner";
      inputs.nixpkgs.follows = "nixpkgs";
      inputs.systems.follows = "systems";
    };
  };

  outputs = {
    self,
    nixpkgs,
    systems,
    ...
  } @ inputs: let
    inherit (nixpkgs) lib;
    eachSystem = lib.genAttrs (import systems);
    pkgsFor = eachSystem (system:
      import nixpkgs {
        localSystem.system = system;
        overlays = with self.overlays; [aquamarine];
      });
    pkgsCrossFor = eachSystem (system: crossSystem:
      import nixpkgs {
        localSystem = system;
        crossSystem = crossSystem;
        overlays = with self.overlays; [aquamarine];
      });
    mkDate = longDate: (lib.concatStringsSep "-" [
      (builtins.substring 0 4 longDate)
      (builtins.substring 4 2 longDate)
      (builtins.substring 6 2 longDate)
    ]);
    version = lib.removeSuffix "\n" (builtins.readFile ./VERSION);
  in {
    overlays = {
      default = self.overlays.aquamarine;

      aquamarine = lib.composeManyExtensions [
        inputs.hyprutils.overlays.default
        inputs.hyprwayland-scanner.overlays.default
        (final: prev: {
          aquamarine = final.callPackage ./nix/default.nix {
            stdenv = final.gcc14Stdenv;
            version = version + "+date=" + (mkDate (self.lastModifiedDate or "19700101")) + "_" + (self.shortRev or "dirty");
          };
          aquamarine-with-tests = final.aquamarine.override {doCheck = true;};
        })
      ];
    };

    packages = eachSystem (system: {
      default = self.packages.${system}.aquamarine;
      inherit (pkgsFor.${system}) aquamarine aquamarine-with-tests;
      aquamarine-cross = (pkgsCrossFor.${system} "aarch64-linux").aquamarine;
    });

    formatter = eachSystem (system: pkgsFor.${system}.alejandra);
  };
}
