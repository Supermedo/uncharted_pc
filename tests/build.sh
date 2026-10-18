#!/usr/bin/env bash
# Builds and runs the C tests (tests/test_*.c) for the current platform, so the
# Windows path is covered as well as Linux:
#
#   bash tests/build.sh          # Linux (or any POSIX shell)
#   bash build.sh --test         # same, via the top-level script
#
# Needs SDL3 and FFmpeg development files, and the LibAtrac9 submodule, because
# the runtime links runtime_pad.c (SDL3) and runtime_ajm.c (FFmpeg, ATRAC9).
# tests/stubs.c supplies the bbgpu_* entry points that live in the GPU library.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
mkdir -p out/tests

case $(uname -s) in
MINGW*|MSYS*)
    # shellcheck source=../msys2-env.sh
    source ./msys2-env.sh
    CC=${CC:-clang}
    # The GPU library is not linked here, so its API-set imports need these.
    libs="-lonecore -lbcrypt -lws2_32 -lpthread"
    # SDL3 must be linked with -mwindows: as a console binary the dummy video
    # driver leaves SDL's virtual gamepad unrecognised and test_pad fails.
    extra="-mwindows"
    # The runtime reserves the PS4 range, which needs a non-relocated image.
    nohigh="-Wl,--disable-high-entropy-va"
    ;;
*)
    CC=${CC:-$(command -v cc || command -v gcc)}
    libs="-lm -pthread"
    extra=
    nohigh=
    ;;
esac

if [[ ! -f third_party/LibAtrac9/C/src/libatrac9.h ]]; then
    echo 'LibAtrac9 is missing: git submodule update --init --recursive' >&2
    exit 1
fi

# Third-party decoders, without this project's -Werror policy.
atrac9=()
for source in third_party/LibAtrac9/C/src/*.c; do
    object=out/tests/atrac9-$(basename "${source%.c}").o
    "$CC" -std=c99 -O1 -w -c "$source" -o "$object"
    atrac9+=("$object")
done

runtime=(src/runtime.c src/runtime_memory.c src/runtime_thread.c src/runtime_fiber.c
         src/runtime_mutex.c src/runtime_sema.c src/runtime_rwlock.c src/runtime_kernel.c
         src/runtime_file.c src/runtime_savedata.c src/runtime_rtc.c src/runtime_time.c
         src/runtime_content.c src/runtime_services.c src/runtime_ajm.c src/runtime_audio.c
         src/runtime_pad.c tests/stubs.c)
runtime+=("${atrac9[@]}")

if [[ ${BB_TEST_MINIMAL:-0} == 1 ]]; then
    # Without SDL3/FFmpeg, drop the modules that need them and stand in for the
    # entry points runtime.c calls into them.
    runtime=(src/runtime.c src/runtime_memory.c src/runtime_thread.c src/runtime_fiber.c
             src/runtime_mutex.c src/runtime_sema.c src/runtime_rwlock.c src/runtime_kernel.c
             src/runtime_file.c src/runtime_savedata.c src/runtime_rtc.c src/runtime_time.c
             src/runtime_content.c src/runtime_services.c src/compat_win.c tests/stubs.c)
    cat > out/tests/audio-stub.c <<'STUB'
/* Stand-ins for the modules dropped in minimal mode. runtime.c calls every
 * subsystem's resolve/report unconditionally, so they must exist. */
#include "../src/runtime.h"
uintptr_t runtime_ajm_resolve(const char *name) { (void)name; return 0; }
void runtime_ajm_report(void) {}
void runtime_ajm_configure(void) {}
uintptr_t runtime_audio_resolve(const char *name) { (void)name; return 0; }
void runtime_audio_report(void) {}
void runtime_audio_configure(void) {}
uintptr_t runtime_pad_resolve(const char *name) { (void)name; return 0; }
void runtime_pad_report(void) {}
void runtime_pad_configure(void) {}
STUB
    runtime+=(out/tests/audio-stub.c "${atrac9[@]}")
else
    runtime+=(src/compat_win.c)
    read -r -a pkg_includes <<< "$(pkg-config --cflags libavcodec libavutil libavformat sdl3)"
    read -r -a pkg_libs <<< "$(pkg-config --libs libavcodec libavutil libavformat sdl3)"
    libs="$libs ${pkg_libs[*]}"
fi

warnings=(-Wall -Wextra)
failed=0

# Runs one built test. $1 is the target name; the .exe suffix is added by the
# toolchain on Windows, so resolve it rather than assuming.
run() {
    local name=$1; shift
    local exe="out/tests/$name"
    [[ -x $exe.exe ]] && exe+=.exe
    if ./"$exe" "$@" > "out/tests/$name.log" 2>&1; then
        # The tests print their own "PASS: ..." line; drop any duplicated prefix.
        printf 'PASS  %s\n' "$(tail -1 "out/tests/$name.log" | sed 's/^PASS: *//')"
    else
        local code=$?
        printf 'FAIL  %s (exit %d)\n' "$name" "$code" >&2
        tail -5 "out/tests/$name.log" >&2
        failed=1
    fi
}

build() {
    local name=$1; shift
    if ! "$CC" -std=c11 -O1 -g "${warnings[@]}" -pthread -I. -Isrc "${pkg_includes[@]+"${pkg_includes[@]}"}" \
        "$@" $libs $extra $nohigh -o "out/tests/$name" 2> "out/tests/$name.build.log"; then
        printf 'BUILD FAILED  %s (out/tests/%s.build.log)\n' "$name" "$name" >&2
        tail -20 "out/tests/$name.build.log" >&2
        failed=1
        return 1
    fi
}

if [[ ${BB_TEST_MINIMAL:-0} == 1 ]]; then
    echo 'Minimal mode: runtime_pad.c and runtime_ajm.c are not linked, so their'
    echo 'tests (test_pad) are skipped. Install SDL3 and FFmpeg for the full set.'
    skipped=(pad-test)
else
    skipped=()
fi

# A build failure counts as a failure, so use `if build ...; then run ...; fi`
# rather than `build ... && run ...`, which would swallow the exit status.
if build sema-test tests/test_sema.c "${runtime[@]}"; then
    run sema-test
    run sema-test --concurrency
    run sema-test --cancel-delete
    run sema-test --priority
fi
if build runtime-test tests/test_runtime.c "${runtime[@]}"; then
    run runtime-test
fi
if build content-test tests/test_content.c src/runtime_content.c tests/stubs.c; then
    run content-test
fi
# runtime_file.c is included directly, so only compat_win.c is needed alongside it.
if build file-mods-test tests/test_file_mods.c src/compat_win.c; then
    run file-mods-test
fi
if [[ " ${skipped[*]} " != *" pad-test "* ]]; then
    if build pad-test tests/test_pad.c tests/stubs.c src/compat_win.c; then
        run pad-test
    fi
fi

if [[ $failed -ne 0 ]]; then
    echo 'C tests failed.' >&2
    exit 1
fi
echo 'C tests: all passed'