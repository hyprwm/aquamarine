{
  lib,
  stdenv,
  cmake,
  hwdata,
  hyprutils,
  hyprwayland-scanner,
  libdisplay-info,
  libdrm,
  libffi,
  libinput,
  libseat,
  mesa,
  pixman,
  pkg-config,
  udev,
  wayland,
  wayland-protocols,
  version ? "git",
  doCheck ? false,
}:
stdenv.mkDerivation {
  pname = "aquamarine";
  inherit version doCheck;
  src = ../.;

  nativeBuildInputs = [
    cmake
    hyprwayland-scanner
    pkg-config
  ];

  buildInputs = [
    hyprutils
    libdisplay-info
    libdrm
    libffi
    libinput
    libseat
    mesa
    pixman
    udev
    wayland
    wayland-protocols
  ];

  depsBuildBuild = [
    hwdata
  ];

  outputs = ["out" "dev"];

  cmakeBuildType = "RelWithDebInfo";

  dontStrip = true;

  meta = {
    homepage = "https://github.com/hyprwm/aquamarine";
    description = "A very light linux rendering backend library";
    license = lib.licenses.bsd3;
    platforms = lib.platforms.linux;
  };
}
