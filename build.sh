#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# Keep the binary's source revision for release packaging. Ignore submodule worktree
# dirt while marking changed tracked project files.
write_version() {
    local version
    version=$(git describe --tags --match 'v*' --always 2>/dev/null || echo unknown)
    git update-index -q --refresh 2>/dev/null || true
    if ! git diff-index --quiet --ignore-submodules=dirty HEAD -- 2>/dev/null; then
        git status --short --untracked-files=no >&2
        version="$version-dirty"
    fi
    printf '%s\n' "$version" > out/VERSION
    echo "Version: $version (out/VERSION)"
}
# Build against a patched copy of the pinned submodule. Never edit the vendor checkout.
prepare_fsr_source() {
    if [[ ! -f gpu/third_party/fsr-vulkan/CMakeLists.txt || ! -f gpu/third_party/imgui/imgui.h ||
          ! -f gpu/third_party/xbyak/xbyak/xbyak.h ]]; then
        git submodule update --init --recursive
    fi
    local revision key patch
    revision=$(git -C gpu/third_party/fsr-vulkan rev-parse HEAD)
    key=$( { printf '%s\n' "$revision"; sha256sum gpu/patches/fsr-vulkan/*.patch; } | sha256sum | cut -c1-16 )
    fsr_source="$PWD/out/fsr-vulkan/$key"
    if [[ ! -f $fsr_source/.patched ]]; then
        mkdir -p "$fsr_source"
        git -C gpu/third_party/fsr-vulkan archive HEAD | tar -x -C "$fsr_source"
        for patch in gpu/patches/fsr-vulkan/*.patch; do
            patch -s -d "$fsr_source" -p1 < "$patch"
        done
        touch "$fsr_source/.patched"
    fi
}
# Windows: MSYS2 CLANG64 (packages in packaging/windows/README.md). The GPU library is static
# there and CMake links bb-probe.exe as well; clang PGO with pgo/merged.profdata, LTO off by default.
case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=msys2-env.sh
    source ./msys2-env.sh
    # MSYS2's Vulkan headers, as on the GitHub runner: an installed Vulkan SDK (VULKAN_SDK) would
    # point CMake at its own, older headers, which lack extensions the port uses.
    unset VULKAN_SDK
    for tool in clang cmake ninja pkg-config; do
        command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (packaging/windows/README.md)." >&2; exit 1; }
    done
    prepare_fsr_source
    # perf7: BB_PGO=generate|use|off (use = pgo/merged.profdata, see gpu/CMakeLists.txt), BB_LTO=ON|OFF.
    # Default: the committed profile when it is there (release builds are PGO builds).
    pgo=${BB_PGO:-}
    if [[ -z $pgo ]]; then
        if [[ -f pgo/merged.profdata ]]; then pgo=use; else pgo=off; fi
    fi
    if [[ $pgo == use && ! -f pgo/merged.profdata ]]; then
        echo 'BB_PGO=use needs pgo/merged.profdata.' >&2; exit 1
    fi
    # libc++ stamps its version into the names of its internal helpers (abi tag "nqe230103" =
    # 23.1.3): with any other clang (the GitHub runner updates MSYS2) none of them matched the
    # profile, two thirds of it (0.3.6.41's release build). The profile gets this compiler's
    # stamp, in a copy under out/pgo; code that really changed in libc++ is still skipped.
    pgo_dir=$PWD/pgo
    if [[ $pgo == use ]]; then
        here=$(echo '#include <version>' | clang++ -x c++ -dM -E - | awk '/define _LIBCPP_VERSION /{print $3}')
        mkdir -p out/pgo
        llvm-profdata merge --text pgo/merged.profdata -o out/pgo/profile.proftext
        # The tag as written (three signature letters, the version), found and replaced as plain
        # text by awk: Git for Windows' grep and sed missed these patterns.
        tag=$(awk 'match($0, /B9[a-z][a-z][a-z][0-9][0-9][0-9][0-9][0-9][0-9]/) {
                       print substr($0, RSTART + 2, 9); exit }' out/pgo/profile.proftext)
        stamp=${tag:3}
        if [[ -n $here && -n $stamp && $here != "$stamp" ]]; then
            awk -v from="B9$tag" -v to="B9${tag:0:3}$here" '{
                     out = ""; while ((i = index($0, from)) > 0) {
                         out = out substr($0, 1, i - 1) to; $0 = substr($0, i + length(from)) }
                     print out $0 }' out/pgo/profile.proftext > out/pgo/stamped.proftext
            llvm-profdata merge out/pgo/stamped.proftext -o out/pgo/new.profdata
            if ! cmp -s out/pgo/new.profdata out/pgo/merged.profdata; then mv out/pgo/new.profdata out/pgo/merged.profdata; fi
            pgo_dir=$PWD/out/pgo
            echo "PGO profile: libc++ names from $stamp renamed for this compiler's $here"
        fi
        rm -f out/pgo/profile.proftext out/pgo/stamped.proftext out/pgo/new.profdata
    fi
    # perf7: BB_OPT=2|3: optimization level of the RelWithDebInfo build (CMake's default is -O2).
    opt="-O${BB_OPT:-2} -g -DNDEBUG"
    cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
        -DBB_PGO_DIR="$pgo_dir" -DBB_LTO="${BB_LTO:-OFF}" -DBB_FSR_VULKAN_SOURCE="$fsr_source" \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS_RELWITHDEBINFO="$opt" -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="$opt" >/dev/null
    echo "GPU library and loader: LTO ${BB_LTO:-OFF}, PGO $pgo, -O${BB_OPT:-2}"
    if ! ninja -C out/gpu bb-probe bb-gpu-capabilities bb-play > out/gpu-build.log 2>&1; then
        awk 'substr($0, 1, 1) != "["' out/gpu-build.log | tail -40 >&2 || true
        echo 'Build failed (full log: out/gpu-build.log)' >&2; exit 1
    fi
    cp out/gpu/bb-probe.exe out/gpu/bb-gpu-capabilities.exe out/gpu/bb-play.exe out/
    if [[ $pgo == use ]]; then
        # How far the code has moved from the profile: clang's per-file counts of functions
        # changed since it was recorded (out of date) or added (no data). Only the files
        # compiled in this build: a full build (CI, a clean out/) gives the whole picture.
        profiled=$(llvm-profdata show pgo/merged.profdata 2>/dev/null | awk '/^Total functions:/ {print $3}')
        # BB_PGO_MAX_UNMATCHED=<percent> (the GitHub build): more than that unmatched fails the
        # build, so a release never ships mostly without its profile.
        awk -v profiled="${profiled:-0}" -v limit="${BB_PGO_MAX_UNMATCHED:-}" '
             /(hash mismatch|no profile data available for function)/ {
                 file=$0; sub(/^warning: /, "", file); sub(/: (no profile data|function control flow).*/, "", file)
                 sub(/^.*\/(gpu|src|tools|packaging)\//, "", file)
                 if ($0 ~ /hash mismatch/) stale++; else added++
                 sf[file]++
             }
             /^\[[0-9]+\/[0-9]+\] / { compiled++ }
             END {
                 if (!compiled) exit
                 printf "PGO profile: %d functions changed and %d are new since the profile, in the %d steps built now", stale, added, compiled
                 if (profiled) printf " (%.1f%% of the %d profiled)", 100*(stale+added)/profiled, profiled
                 printf "\n"
                 for (f in sf) printf "%d %s\n", sf[f], f | "sort -rn | head -5 | sed \"s/^/  /\""
                 close("sort -rn | head -5 | sed \"s/^/  /\"")
                 if (limit != "" && profiled && 100*(stale+added)/profiled > limit+0) {
                     printf "PGO profile: more than %s%% unmatched (BB_PGO_MAX_UNMATCHED): build stopped\n", limit
                     exit 3
                 }
             }' out/gpu-build.log || exit 1
    fi
    write_version
    echo "Built $PWD/out/bb-probe.exe"
    exit 0
    ;;
esac
if [[ -z ${CC:-} ]]; then
    CC=$(command -v cc || command -v gcc || true)
    if [[ -z $CC ]]; then
        for candidate in /nix/store/*-gcc-wrapper-*/bin/gcc; do
            if [[ -x $candidate ]]; then CC=$candidate; break; fi
        done
    fi
fi
if [[ -z ${CC:-} ]]; then echo 'Install GCC/Clang or set CC.' >&2; exit 1; fi
# Dependencies come from pkg-config (Vulkan loader/headers, SDL3 and FFmpeg). On NixOS the
# environment is provided by shell.nix; re-enter it automatically if needed.
if ! { command -v pkg-config >/dev/null && pkg-config --exists vulkan sdl3 libavcodec libavutil && command -v cmake >/dev/null && command -v ninja >/dev/null; }; then
    if [[ -z ${BB_IN_NIX_SHELL:-} ]] && command -v nix-shell >/dev/null; then
        exec env BB_IN_NIX_SHELL=1 nix-shell shell.nix --run "bash build.sh $*"
    fi
    echo 'Need pkg-config with vulkan, sdl3, libavcodec and libavutil, cmake and ninja (see shell.nix).' >&2; exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3 libavcodec libavutil)"
# Newer Vulkan headers than distributions ship (third_party/Vulkan-Headers; the video core uses
# VK_VALVE_shader_mixed_float_dot_product and other recent names).
[[ -f third_party/Vulkan-Headers/include/vulkan/vulkan.h ]] || git submodule update --init third_party/Vulkan-Headers
vulkan_headers=$PWD/third_party/Vulkan-Headers/include
includes=(-I"$vulkan_headers" "${includes[@]}")
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3 libavcodec libavutil)"
# GPU library (shadPS4 video core + drivers), built by CMake into out/gpu/libbbgpu.so.
# BB_PGO: generate (instrumented build that writes pgo/ while the game runs), use, off.
# Default: use the profile in pgo/ when there is one. BB_LTO=OFF disables link-time optimization.
pgo=${BB_PGO:-}
if [[ -z $pgo ]]; then
    if [[ -n $(find pgo -name '*.gcda' -print -quit 2>/dev/null) ]]; then pgo=use; else pgo=off; fi
fi
mkdir -p pgo
prepare_fsr_source
cmake -S gpu -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" \
    -DBB_LTO="${BB_LTO:-ON}" -DBB_PGO_DIR="$PWD/pgo" -DVulkan_INCLUDE_DIR="$vulkan_headers" \
    -DBB_FSR_VULKAN_SOURCE="$fsr_source" >/dev/null
echo "GPU library: PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja ${BB_JOBS:+-j"$BB_JOBS"} -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^\[' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2; exit 1
fi
# $ORIGIN/gpu: packaged copies keep the library next to the binary without patching it.
gpu=(-Lout/gpu -lbbgpu -Wl,-rpath,'$ORIGIN/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic)
runtime=(src/runtime*.c)
# Third-party decoders: compiled once, without this project's -Werror policy.
atrac9=(third_party/LibAtrac9/C/src/*.c)
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in "${atrac9[@]}"; do "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"; done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc src/probe.c src/cpu_patch.c "${runtime[@]}" src/vulkan_smoke.c out/libatrac9.a -lm -lZydis "${gpu[@]}" "${libraries[@]}" -o out/bb-probe
echo "Built $PWD/out/bb-probe"
# GPU check for run.sh (live_resolution=auto): links only the Vulkan loader.
"$CC" -std=c11 -O2 -Wall -Wextra -Werror tools/gpu_capabilities.c "${libraries[@]}" -o out/bb-gpu-capabilities
write_version
if [[ ${1:-} == --test ]]; then
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread "${includes[@]}" -I. -Isrc tests/test_pad.c "${libraries[@]}" -o out/pad-test
    out/pad-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_runtime.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/runtime-test
    out/runtime-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_file_mods.c -o out/file-mods-test
    out/file-mods-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_sema.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/sema-test
    out/sema-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc tests/test_ajm.c "${runtime[@]}" out/libatrac9.a -lm "${gpu[@]}" "${libraries[@]}" -o out/ajm-test
    out/ajm-test
    "$CC" -std=c11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -I. -Isrc tests/test_content.c src/runtime_content.c -o out/content-test
    out/content-test
    "$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -Isrc tests/test_cpu_patch.c src/cpu_patch.c -lZydis -o out/cpu-patch-test
    out/cpu-patch-test
fi
