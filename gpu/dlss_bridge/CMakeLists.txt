# SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
# SPDX-License-Identifier: MIT
#
# The DLSS bridge links the NVIDIA DLSS SDK and is built separately from the port.
# Windows (bbport_dlss.dll, MSVC only):
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> -DVULKAN_INCLUDE=<Vulkan-Headers include>
#   cmake --build out/dlss-bridge --config Release
# Linux (libbbport_dlss.so, GCC or Clang; packaging/linux/build_dlss.sh):
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> -DVULKAN_INCLUDE=<Vulkan-Headers include>
#   ninja -C out/dlss-bridge
# Ship it next to bb-probe with lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.<version> from the SDK.

cmake_minimum_required(VERSION 3.20)
project(bbport_dlss LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The DLSS bridge is 64-bit only")
endif()
if (NOT WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The DLSS bridge is Windows x64 or Linux x86_64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(bbport_dlss SHARED bbport_dlss_bridge.cpp bbport_dlss_bridge.h)
target_include_directories(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/include" "${VULKAN_INCLUDE}")
if (WIN32)
    # nvsdk_ngx_s.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
    set_property(TARGET bbport_dlss PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded")
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
    target_compile_definitions(bbport_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # libnvsdk_ngx.a loads the driver's libnvidia-ngx.so.1 and the DLSS snippet at run time.
    set_target_properties(bbport_dlss PROPERTIES CXX_VISIBILITY_PRESET hidden
                          POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(bbport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Linux_x86_64/libnvsdk_ngx.a"
                          ${CMAKE_DL_LIBS})
    target_link_options(bbport_dlss PRIVATE -Wl,--no-undefined -static-libstdc++ -static-libgcc)
endif()
