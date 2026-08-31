cmake_minimum_required(VERSION 3.20)
project(flare_diag CXX)

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

add_library(flare_diag MODULE
    flare_diag_proxy.cpp
    exports.def
)

set_target_properties(flare_diag PROPERTIES
    OUTPUT_NAME "d3d8"
    PREFIX ""
)

target_include_directories(flare_diag PRIVATE
    "${D3D8_SDK_PATH}"
)

target_compile_definitions(flare_diag PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(flare_diag PRIVATE /W4 /WX /wd4191 /wd4996 /permissive- /EHa)
    target_link_options(flare_diag PRIVATE /SAFESEH:NO)
endif()
