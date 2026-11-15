# Build environment: `nix-shell native_probe/shell.nix --run 'bash native_probe/run.sh'`
{ pkgs ? import <nixpkgs> {} }:
pkgs.mkShell {
  packages = with pkgs; [
    gcc gnumake cmake ninja pkg-config python3 binutils
    vulkan-headers vulkan-loader sdl3
    # GPU library (gpu/): shadPS4 video core dependencies
    ffmpeg-headless boost fmt magic-enum robin-map xxhash vulkan-memory-allocator glslang spirv-cross xbyak zydis spirv-headers miniz libx11 libxcb libxau libxdmcp xorgproto wayland
    # Online features (gpu/CMakeLists.txt): protobuf, JSON, TLS and UPnP
    protobuf nlohmann_json openssl zlib miniupnpc
  ];
}
