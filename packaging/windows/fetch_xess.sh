#!/usr/bin/env bash
# Fetches Intel's libxess.dll (XeSS Super Resolution, Intel Simplified Software License, which
# allows redistribution of the binary with the license) from the official SDK release and puts it
# with the license texts in out/, next to bb-probe.exe; package.sh ships them. The renderer loads
# the DLL at run time (vk_xess.cpp); without it XeSS stays hidden in the launcher.
#   bash packaging/windows/fetch_xess.sh
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
version=3.0.2
sha256=88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990
url=https://github.com/intel/xess/releases/download/v$version/XeSS_SDK_$version.zip
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p out
curl -fL --retry 3 -o "$tmp/sdk.zip" "$url"
echo "$sha256  $tmp/sdk.zip" | sha256sum -c -
unzip -q "$tmp/sdk.zip" bin/libxess.dll LICENSE.txt third-party-programs.txt -d "$tmp/sdk"
cp "$tmp/sdk/bin/libxess.dll" out/libxess.dll
cp "$tmp/sdk/LICENSE.txt" out/Intel-XeSS-LICENSE.txt
cp "$tmp/sdk/third-party-programs.txt" out/Intel-XeSS-third-party-programs.txt
echo "XeSS $version ready: out/libxess.dll (needs the Visual C++ 2022 x64 Redistributable on the PC)"
