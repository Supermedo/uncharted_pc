#!/usr/bin/env bash
# Linux fork: NVIDIA DLSS (DLAA) for the Linux build. Fetches the NVIDIA DLSS SDK (headers, the
# static NGX loader and the Linux DLSS library; github.com/NVIDIA/DLSS, NVIDIA's license), builds
# the bridge (gpu/dlss_bridge) as libbbport_dlss.so and puts both in out/dlss, where
# packaging/linux/package.sh picks them up. Run after `bash build.sh`.
# DLSS_SDK_DIR: an existing SDK checkout instead of out/dlss-sdk.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
sdk=${DLSS_SDK_DIR:-out/dlss-sdk}
version=310.9.16
if [[ ! -f $sdk/include/nvsdk_ngx_vk.h || ! -f $sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.$version ||
      ! -f $sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.$version ]]; then
    rm -rf "$sdk"
    git clone --filter=blob:none --no-checkout --depth 1 --branch "v$version" https://github.com/NVIDIA/DLSS "$sdk"
    git -C "$sdk" sparse-checkout set --no-cone '/include/*' '/lib/Linux_x86_64/libnvsdk_ngx.a' \
        "/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.*" "/lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.*" \
        '/LICENSE.txt'
    git -C "$sdk" checkout
fi
library=$(ls "$sdk"/lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.* | sort -V | tail -1)
# Frame Generation (RTX 40 and newer): NGX finds it next to the bridge like the DLSS library.
library_fg=$(ls "$sdk"/lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.* | sort -V | tail -1)
[[ -f third_party/Vulkan-Headers/include/vulkan/vulkan.h ]] || git submodule update --init third_party/Vulkan-Headers
cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DDLSS_SDK_ROOT="$(realpath "$sdk")" -DVULKAN_INCLUDE="$PWD/third_party/Vulkan-Headers/include" >/dev/null
ninja -C out/dlss-bridge >/dev/null
mkdir -p out/dlss
rm -f out/dlss/libnvidia-ngx-dlss.so.* out/dlss/libnvidia-ngx-dlssg.so.*
cp out/dlss-bridge/libbbport_dlss.so "$library" "$library_fg" out/dlss/
cp "$sdk/LICENSE.txt" out/dlss/LICENSE-DLSS-SDK.txt
# Running from the source tree (python3 launcher/uncharted_launcher.py): next to out/bb-probe too.
if [[ -f out/bb-probe ]]; then cp out/dlss/libbbport_dlss.so "$library" "$library_fg" out/; fi
echo "DLSS: out/dlss/libbbport_dlss.so, $(basename "$library") and $(basename "$library_fg")"
