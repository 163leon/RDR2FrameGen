cmake_minimum_required(VERSION 3.25)
project(RDR2FrameGen LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "RDR2FrameGen builds with MSVC for x64 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)

# MinHook (BSD-2-Clause), built from source.
add_library(minhook STATIC
    third_party/minhook/src/buffer.c
    third_party/minhook/src/hook.c
    third_party/minhook/src/trampoline.c
    third_party/minhook/src/hde/hde64.c
)
target_include_directories(minhook PUBLIC third_party/minhook/include)
target_compile_options(minhook PRIVATE /W0)

# NVIDIA Streamline SDK: headers and the import library of sl.interposer.dll. It is not part of this
# repository; tools/fetch-streamline.ps1 downloads NVIDIA's official release into external/streamline.
set(STREAMLINE_SDK "${CMAKE_CURRENT_SOURCE_DIR}/external/streamline" CACHE PATH "NVIDIA Streamline SDK folder (include/, lib/, bin/)")
if(NOT EXISTS "${STREAMLINE_SDK}/include/sl.h")
    message(FATAL_ERROR "Streamline SDK not found in ${STREAMLINE_SDK}. Run tools/fetch-streamline.ps1 or pass -DSTREAMLINE_SDK=<folder>.")
endif()

# Vulkan headers only; nothing links against vulkan-1. Taken from an installed Vulkan SDK, otherwise
# downloaded from Khronos.
set(VULKAN_HEADERS "$ENV{VULKAN_SDK}/Include" CACHE PATH "Folder that contains vulkan/vulkan.h")
if(NOT EXISTS "${VULKAN_HEADERS}/vulkan/vulkan.h")
    include(FetchContent)
    FetchContent_Declare(vulkan_headers
        URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.309.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(vulkan_headers)
    set(VULKAN_HEADERS "${vulkan_headers_SOURCE_DIR}/include" CACHE PATH "" FORCE)
endif()

# Import library for ScriptHookRDR2.dll, generated from the list of the exports the mod calls.
set(SCRIPTHOOK_LIB "${CMAKE_CURRENT_BINARY_DIR}/ScriptHookRDR2.lib")
add_custom_command(
    OUTPUT "${SCRIPTHOOK_LIB}"
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_SOURCE_DIR}/src/scripthook.def" "/out:${SCRIPTHOOK_LIB}"
    DEPENDS src/scripthook.def
    VERBATIM)
add_custom_target(scripthook_lib DEPENDS "${SCRIPTHOOK_LIB}")

add_library(RDR2FrameGen SHARED
    src/camera.cpp
    src/dllmain.cpp
    src/log.cpp
    src/config.cpp
    src/crash_log.cpp
    src/frames.cpp
    src/hud.cpp
    src/ngx_tap.cpp
    src/sl_runtime.cpp
    src/vk_route.cpp
)
add_dependencies(RDR2FrameGen scripthook_lib)
set_target_properties(RDR2FrameGen PROPERTIES SUFFIX ".asi")
target_include_directories(RDR2FrameGen PRIVATE src)
target_include_directories(RDR2FrameGen SYSTEM PRIVATE
    "${STREAMLINE_SDK}/include"
    "${STREAMLINE_SDK}/external/ngx-sdk/include"
    "${VULKAN_HEADERS}"
)
target_compile_options(RDR2FrameGen PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus /EHsc /external:anglebrackets /external:W0)
# sl.interposer.dll lives in <game>\RDR2FrameGen\, not on the DLL search path: it is delay-loaded and
# the mod loads it by full path before the first Streamline call.
target_link_libraries(RDR2FrameGen PRIVATE minhook "${SCRIPTHOOK_LIB}" "${STREAMLINE_SDK}/lib/x64/sl.interposer.lib" delayimp)
target_link_options(RDR2FrameGen PRIVATE /DELAYLOAD:sl.interposer.dll)
