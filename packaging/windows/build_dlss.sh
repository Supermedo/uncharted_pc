#!/usr/bin/env bash
# Builds bbport_dlss.dll (gpu/dlss_bridge, the only code that uses the NVIDIA DLSS SDK) with
# MSVC and puts it with NVIDIA's nvngx_dlss.dll (Super Resolution) and nvngx_dlssg.dll (Frame
# Generation) next to out/bb-probe.exe; package.sh ships them.
# Needs Visual Studio 2022 (Build Tools) and a checkout of https://github.com/NVIDIA/DLSS:
#   DLSS_SDK_ROOT=F:/sdk/DLSS bash packaging/windows/build_dlss.sh
# Without Visual Studio, a portable MSVC (cl.exe + Windows SDK) and its environment script, built
# with Ninja: MSVC_SETUP=F:/sdk/msvc/setup_x64.bat. MSYS2_ROOT as in msys2-env.sh.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_ROOT:?Set DLSS_SDK_ROOT to a checkout of github.com/NVIDIA/DLSS}
cmake=${CMAKE_EXE:-cmake}
msys2_root=${MSYS2_ROOT:-/c/msys64}
# Only the Vulkan headers: MSYS2's C headers must not reach MSVC.
rm -rf out/vkinclude && mkdir -p out/vkinclude
cp -r "$msys2_root/clang64/include/vulkan" "$msys2_root/clang64/include/vk_video" out/vkinclude/
if [[ -n ${MSVC_SETUP:-} ]]; then
    # cmd runs the environment script, then CMake with Ninja and cl in that environment.
    rm -rf out/dlss-bridge && mkdir -p out/dlss-bridge
    cmake_w=$(cygpath -w "$(command -v "$cmake")")
    cat > out/dlss-bridge/build.bat <<EOF
@echo off
call "$(cygpath -w "$MSVC_SETUP")" || exit /b 1
"$cmake_w" -S "$(cygpath -w "$PWD/gpu/dlss_bridge")" -B "$(cygpath -w "$PWD/out/dlss-bridge")" -G Ninja ^
    -DCMAKE_MAKE_PROGRAM="$(cygpath -m "$(command -v ninja)")" -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_CXX_COMPILER=cl ^
    -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")" || exit /b 1
"$cmake_w" --build "$(cygpath -w "$PWD/out/dlss-bridge")" || exit /b 1
EOF
    cmd //c "$(cygpath -w "$PWD/out/dlss-bridge/build.bat")"
    bridge=out/dlss-bridge/bbport_dlss.dll
else
    "$cmake" -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 \
        -DDLSS_SDK_ROOT="$(cygpath -m "$sdk")" -DVULKAN_INCLUDE="$(cygpath -m "$PWD/out/vkinclude")"
    "$cmake" --build out/dlss-bridge --config Release
    bridge=out/dlss-bridge/Release/bbport_dlss.dll
fi
cp "$bridge" "$sdk/lib/Windows_x86_64/rel/nvngx_dlss.dll" \
    "$sdk/lib/Windows_x86_64/rel/nvngx_dlssg.dll" out/
cp "$sdk/LICENSE.txt" out/NVIDIA-DLSS-LICENSE.txt
echo "DLSS bridge ready: out/bbport_dlss.dll, out/nvngx_dlss.dll, out/nvngx_dlssg.dll"
