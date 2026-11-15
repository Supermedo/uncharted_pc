# Package the prebuilt Linux port and its Nix runtime closure as an AppImage.
{ pkgs ? import <nixpkgs> { }
, runtimePaths ? (if builtins.pathExists ./runtime-paths.nix then import ./runtime-paths.nix else [ ])
}:
let
  lib = pkgs.lib;
  root = ./..;
  wanted = [
    "run.sh" "LICENSE" "scripts" "patches" "launcher" "launcher/bbport_vulkan.py"
    "out" "out/bb-probe" "out/bb-gpu-capabilities" "out/gpu"
    "out/gpu/libbbgpu.so" "out/VERSION"
  ];
  src = builtins.path {
    name = "uncharted-src";
    path = root;
    filter = path: type:
      let rel = lib.removePrefix (toString root + "/") (toString path);
      in builtins.elem rel wanted
        || lib.hasPrefix "scripts/" rel
        || lib.hasPrefix "patches/" rel;
  };
  icds = lib.concatMapStringsSep ":" (name: "${pkgs.mesa}/share/vulkan/icd.d/${name}")
    [ "radeon_icd.x86_64.json" "intel_icd.x86_64.json" ];
in
pkgs.stdenv.mkDerivation {
  pname = "uncharted";
  version = "0.1";
  inherit src;
  nativeBuildInputs = [ pkgs.makeShellWrapper ];
  buildInputs = map builtins.storePath runtimePaths;
  dontBuild = true;
  dontConfigure = true;
  dontPatchELF = true;
  installPhase = ''
    runHook preInstall
    d=$out/share/uncharted
    mkdir -p $d/bin $d/launcher $out/bin $out/libexec
    cp run.sh LICENSE out/VERSION $d/
    cp -r scripts patches $d/
    cp launcher/bbport_vulkan.py $d/launcher/
    install -m755 out/bb-probe out/bb-gpu-capabilities $d/bin/
    install -Dm755 out/gpu/libbbgpu.so $d/bin/gpu/libbbgpu.so
    runHook postInstall
  '';
  postFixup = ''
    gcc -O2 -Wall -Werror -static -L${pkgs.glibc.static}/lib \
      -DTARGET="\"$out/libexec/uncharted\"" ${./bbport-entry.c} -o $out/bin/uncharted
    makeShellWrapper ${pkgs.python3}/bin/python3 $out/libexec/uncharted \
      --add-flags "$out/share/uncharted/launcher/bbport_vulkan.py ${pkgs.bash}/bin/bash $out/share/uncharted/run.sh" \
      --set BB_BUNDLED_VK_DRIVER_FILES ${icds} \
      --prefix PATH : ${lib.makeBinPath [ pkgs.bash pkgs.coreutils pkgs.util-linux pkgs.python3 ]} \
      --run 'export BB_DATA_DIR=''${BB_DATA_DIR:-''${XDG_DATA_HOME:-$HOME/.local/share}/uncharted}; mkdir -p "$BB_DATA_DIR"'
  '';
  meta.mainProgram = "uncharted";
}
