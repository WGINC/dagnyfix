cmake_minimum_required(VERSION 3.20)
project(decalfix CXX)

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

add_library(decalfix MODULE
    src/decalfix_proxy.cpp
    src/exports.def
)

set_target_properties(decalfix PROPERTIES
    OUTPUT_NAME "d3d8"
    PREFIX ""
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(decalfix PRIVATE
    "${D3D8_SDK_PATH}"
)

target_compile_definitions(decalfix PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if (MSVC)
    target_compile_options(decalfix PRIVATE /std:c++17 /W4 /WX /wd4191 /wd4996 /permissive- /EHa)
    target_link_options(decalfix PRIVATE /SAFESEH:NO)
endif()
