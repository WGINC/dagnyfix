cmake_minimum_required(VERSION 3.20)
project(dagnyfix CXX)

if (NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "This must compile as a 32-bit (x86) target: FFXI is a 32-bit process. "
        "With the Visual Studio generator, configure with -A Win32; with Ninja (VS's CMake integration "
        "default), select the x86 architecture/toolset for this configuration instead. Detected pointer "
        "size: ${CMAKE_SIZEOF_VOID_P} bytes.")
endif()

set(D3D8_SDK_PATH "" CACHE PATH "Path to a directory containing d3d8/includes/d3d8.h")
if (NOT EXISTS "${D3D8_SDK_PATH}/d3d8/includes/d3d8.h")
    message(FATAL_ERROR "Set D3D8_SDK_PATH to a directory containing d3d8/includes/d3d8.h (an Ashita SDK checkout has one).")
endif()

# Dagnyfix is the aura-glow fix (windower/src/windower_core.cpp) and the
# flare-billboard fix (dagnyfix/src/dagnyfix_proxy.cpp, ported from
# flarefix/flarefix_proxy.cpp v3.1) merged into a single proxy DLL. See
# dagnyfix/README.md for why this is safe to merge and how hook ordering
# keeps the two fixes from interfering with each other.
add_library(dagnyfix MODULE
    src/dagnyfix_proxy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../windower/src/windower_core.cpp
    src/exports.def
)

set_target_properties(dagnyfix PROPERTIES
    OUTPUT_NAME "d3d8"
    PREFIX ""
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(dagnyfix PRIVATE
    "${D3D8_SDK_PATH}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../windower/src"
)

target_compile_definitions(dagnyfix PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(dagnyfix PRIVATE /std:c++17 /W4 /WX /wd4191 /wd4996 /permissive- /EHa)
    target_link_options(dagnyfix PRIVATE /SAFESEH:NO)
endif()
