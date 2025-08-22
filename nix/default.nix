{
  lib,
  stdenv,
  stdenvAdapters,
  cmake,
  hwdata,
  hyprutils,
  hyprwayland-scanner,
  libdisplay-info,
  libdrm,
  libffi,
  libGL,
  libinput,
  libgbm,
  pixman,
  pkg-config,
  seatd,
  udev,
  wayland,
  wayland-protocols,
  wayland-scanner,
  version ? "git",
  doCheck ? false,
  debug ? false,
  # whether to use the mold linker
  # disable this for older machines without SSE4_2 and AVX2 support
  withMold ? true,
}: let
  inherit (builtins) foldl';
  inherit (lib.lists) flatten;

  adapters = flatten [
    (lib.optional withMold stdenvAdapters.useMoldLinker)
    (lib.optional debug stdenvAdapters.keepDebugInfo)
  ];

  customStdenv = foldl' (acc: adapter: adapter acc) stdenv adapters;
in
  customStdenv.mkDerivation {
    pname = "aquamarine";
    inherit version doCheck;
    src = ../.;

    strictDeps = true;

    depsBuildBuild = [
      pkg-config
    ];

    nativeBuildInputs = [
      cmake
      hyprwayland-scanner
      pkg-config
    ];

    buildInputs = [
      hwdata
      (hyprutils.override {inherit withMold;})
      libdisplay-info
      libdrm
      libffi
      libGL
      libinput
      libgbm
      pixman
      seatd
      udev
      wayland
      wayland-protocols
      wayland-scanner
    ];

    outputs = ["out" "dev"];

    cmakeBuildType =
      if debug
      then "Debug"
      else "RelWithDebInfo";

    meta = {
      homepage = "https://github.com/hyprwm/aquamarine";
      description = "A very light linux rendering backend library";
      license = lib.licenses.bsd3;
      platforms = lib.platforms.linux;
    };
  }
