{
  lib,
  self,
  inputs,
  ...
}:
let

  mkDate =
    longDate:
    (lib.concatStringsSep "-" [
      (builtins.substring 0 4 longDate)
      (builtins.substring 4 2 longDate)
      (builtins.substring 6 2 longDate)
    ]);
  version = lib.removeSuffix "\n" (builtins.readFile ../VERSION);
in
{
  default = self.overlays.aquamarine;

  aquamarine-with-deps = lib.composeManyExtensions [
    inputs.hyprutils.overlays.default
    inputs.hyprwayland-scanner.overlays.default
    self.overlays.aquamarine
  ];

  aquamarine = final: prev: {
    aquamarine = final.callPackage ./default.nix {
      stdenv = final.gcc15Stdenv;
      version =
        version
        + "+date="
        + (mkDate (self.lastModifiedDate or "19700101"))
        + "_"
        + (self.shortRev or "dirty");
    };
    aquamarine-with-tests = final.aquamarine.override { doCheck = true; };
  };
}
