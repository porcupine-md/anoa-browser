{ lib
, stdenv
, cmake
, qt6
, nix-gitignore
, version ? "0.0.0-dev"
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "anoa";
  inherit version;

  # The repository itself, minus what a build does not need. Without the filter
  # every local build/ directory and node_modules tree becomes part of the
  # derivation hash, so a rebuild after running the tests would look like a
  # source change.
  src = nix-gitignore.gitignoreSource [
    "build/"
    "build-*/"
    "result"
    "pages/"
    "docs/"
    ".github/"
    "tests/*/node_modules/"
  ] ../.;

  nativeBuildInputs = [
    cmake
    qt6.wrapQtAppsHook
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtwebengine
    qt6.qtwebsockets
  ];

  cmakeFlags = [
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
    # The tag is the single source of truth for the version everywhere else, so
    # it is passed in here rather than left to the default in CMakeLists.txt.
    (lib.cmakeFeature "ANOA_VERSION_OVERRIDE" version)
    # Qt lives at an absolute path in the store, not in ./lib beside the binary,
    # so the bundle RPATH would leave nothing findable. See CMakeLists.txt.
    (lib.cmakeBool "PORTABLE_RPATH" false)
  ];

  # Enough to prove the binary links and starts. Anything that renders a page
  # belongs outside the build sandbox, where QtWebEngine's zygote can have the
  # namespaces it wants.
  #
  # offscreen because --version still constructs the application object, and a
  # build has no display to connect to — without it this fails for a reason that
  # has nothing to do with whether the build worked.
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    QT_QPA_PLATFORM=offscreen ./${if stdenv.hostPlatform.isDarwin then "anoa.app/Contents/MacOS/anoa" else "anoa"} --version
    runHook postCheck
  '';

  postInstall = lib.optionalString stdenv.hostPlatform.isDarwin ''
    mkdir -p $out/bin
    ln -s $out/anoa.app/Contents/MacOS/anoa $out/bin/anoa
  '';

  meta = {
    description = "A browser you drive from the command line";
    longDescription = ''
      anoa is a real Chromium in one binary, driven from a shell, an HTTP
      endpoint, or any CDP client. It exposes Chrome-compatible discovery
      endpoints so Playwright and Puppeteer connect unmodified, a /render/*
      family for driving it over plain HTTP including a live view you can put in
      an iframe, and `anoa terminal`, which renders the live page in a terminal
      over SSH.
    '';
    homepage = "https://github.com/porcupine-md/anoa-browser";
    license = lib.licenses.mit;
    mainProgram = "anoa";
    # Narrower than Qt's own list on purpose: qtwebengine in nixpkgs is not
    # built for x86_64-darwin, so an Intel Mac cannot get here. Homebrew ships a
    # universal build for those.
    platforms = [ "x86_64-linux" "aarch64-linux" "aarch64-darwin" ];
  };
})
