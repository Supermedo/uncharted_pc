// Force-included into every C++ file of the GPU library on Windows/MSYS2 CLANG64. Recent libc++
// releases stopped including these standard headers transitively; the bundled sources (sirit,
// FidelityFX SDK, shadPS4) were written against older ones.
#pragma once
#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#endif
