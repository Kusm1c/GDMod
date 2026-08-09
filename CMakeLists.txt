cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(Pathfinder VERSION 1.0.0)
# Add all source files inside src (recursively), excluding src/gdp-2.2 — that's
# reference-only decompiled physics pseudocode (named/typed camila314/gdp source),
# not real buildable code (uses Ghidra/IDA-style types, no real headers).
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER SOURCES EXCLUDE REGEX "src/gdp-2\\.2/")

# Set up the mod binary
add_library(${PROJECT_NAME} SHARED ${SOURCES})

# src/sim is a standalone physics library (the offline validation tools build it
# with a bare compiler). In the mod build, route its numeric parsing / logging
# shims (see src/sim/util.hpp) through Geode's APIs.
target_compile_definitions(${PROJECT_NAME} PRIVATE GDSIM_USE_GEODE)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Set up dependencies, resources, and link Geode.
setup_geode_mod(${PROJECT_NAME})
