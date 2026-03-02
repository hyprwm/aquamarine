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

  outputs =
    {
      self,
      nixpkgs,
      systems,
      ...
    }@inputs:
    let
      inherit (nixpkgs) lib;
      eachSystem = lib.genAttrs (import systems);
      pkgsFor = eachSystem (
        system:
        import nixpkgs {
          localSystem.system = system;
          overlays = with self.overlays; [ aquamarine-with-deps ];
        }
      );
      pkgsCrossFor = eachSystem (
        system: crossSystem:
        import nixpkgs {
          localSystem = system;
          crossSystem = crossSystem;
          overlays = with self.overlays; [ aquamarine-with-deps ];
        }
      );
    in
    {
      overlays = import ./nix/overlays.nix { inherit lib self inputs; };

      packages = eachSystem (system: {
        default = self.packages.${system}.aquamarine;
        inherit (pkgsFor.${system}) aquamarine aquamarine-with-tests;
        aquamarine-cross = (pkgsCrossFor.${system} "aarch64-linux").aquamarine;
      });

      formatter = eachSystem (system: pkgsFor.${system}.nixfmt-tree);
    };
}
