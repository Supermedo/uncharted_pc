/* Small portability helpers shared by the C tests, so the same test sources build
 * on Linux and on the MSYS2 CLANG64 toolchain (packaging/windows/README.md).
 *
 * Two things differ:
 *  - a scratch directory: "/tmp" is a POSIX path, and a native Windows build has
 *    no such root;
 *  - linking a file: POSIX uses symlink(), which Windows only allows with
 *    Developer Mode or Administrator. scripts/mods.py already falls back to hard
 *    links there, so the tests do the same and still exercise the read-only asset
 *    path in runtime_file.c. */
#ifndef BB_TEST_PORTABLE_H
#define BB_TEST_PORTABLE_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
/* Longest path the helpers below build; also the scratch directory size. */
#define BB_TEST_PATH_MAX 512
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <io.h>
#define bb_mkdtemp(target) bb_test_mkdtemp(target)
#else
#include <unistd.h>
#include <sys/stat.h>
#define bb_mkdtemp mkdtemp
#endif

#ifdef _WIN32
/* mkdtemp for native Windows, under the user's temp directory so the path never
 * needs admin rights. Returns target, or NULL. */
static char *bb_test_mkdtemp(char *target) {
    const char *base = getenv("TEMP");
    if (!base) base = getenv("TMP");
    if (!base) base = ".";
    for (unsigned attempt = 0; attempt < 256; ++attempt) {
        _snprintf(target, BB_TEST_PATH_MAX, "%s\\bbport-test-%u-%u", base,
                  (unsigned)_getpid(), attempt);
        /* The suffix makes the name unique, so an existing one is a real clash. */
        if (!_mkdir(target)) return target;
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}
#endif

/* Links source to link. A symlink where one is available, otherwise a hard link
 * (files only). Returns 0 on success. */
static int bb_test_link(const char *source, const char *link) {
#ifdef _WIN32
    /* A directory symlink needs the privilege too; use a junction there. */
    DWORD attributes = GetFileAttributesA(source);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        char command[MAX_PATH * 3];
        _snprintf(command, sizeof(command), "cmd /C mklink /J \"%s\" \"%s\"", link, source);
        return system(command);
    }
    return CreateHardLinkA(link, source, NULL) ? 0 : -1;
#else
    return symlink(source, link);
#endif
}

#endif