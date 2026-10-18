{ lib, stdenv, pkg-config, libevdev }:

stdenv.mkDerivation {
  pname = "trackpoint-3d";
  version = "unstable";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./trackpoint_3d.cpp
      ./config.hpp
      ./config.cpp
      ./input_device.hpp
      ./configure.cpp
      ./packaging
    ];
  };

  nativeBuildInputs = [ pkg-config ];
  buildInputs = [ libevdev ];
  dontConfigure = true;

  buildPhase = ''
    runHook preBuild
    warnings="-Wall -Wextra -Wpedantic -Werror -Wstring-compare -Wconversion -Wsign-conversion -Wshadow -Wformat=2 -Wundef -Wcast-qual -Wnull-dereference -Wduplicated-cond -Wduplicated-branches -Wlogical-op"
    $CXX -std=c++20 -O2 $warnings trackpoint_3d.cpp config.cpp \
      $(pkg-config --cflags --libs libevdev) -o trackpoint-3d
    $CXX -std=c++20 -O2 $warnings configure.cpp config.cpp \
      $(pkg-config --cflags --libs libevdev) -o trackpoint-3d-configure
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 trackpoint-3d "$out/libexec/trackpoint-3d"
    install -Dm755 trackpoint-3d-configure "$out/bin/trackpoint-3d-configure"
    install -Dm644 packaging/trackpoint-3d.service \
      "$out/share/trackpoint-3d/trackpoint-3d.service"
    runHook postInstall
  '';

  meta = {
    description = "Emulate a six-axis mouse using a relative pointing device and keyboard";
    platforms = lib.platforms.linux;
    mainProgram = "trackpoint-3d-configure";
  };
}
