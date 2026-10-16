// Force-included into the FSR-Vulkan (FidelityFX SDK 1.1.4) targets on Windows/MSYS2 CLANG64.
// The SDK is written for MSVC there and relied on headers that recent libc++/mingw-w64 no longer
// pull in transitively (_countof, swprintf_s, log2, placement new).
#pragma once
#ifdef __cplusplus
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <cstdio>
#include <new>
#include <mutex>
#ifndef _countof
template <typename T, size_t N> char (&ffx_countof_helper(T (&)[N]))[N];
#define _countof(a) (sizeof(ffx_countof_helper(a)))
#endif
#if !defined(MINGW_HAS_SECURE_API) || !MINGW_HAS_SECURE_API
#define swprintf_s swprintf
#endif
#endif
