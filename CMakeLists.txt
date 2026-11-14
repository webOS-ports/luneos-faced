# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>

cmake_minimum_required(VERSION 3.10)

project(luneos-faced CXX)

include(webOS/webOS)
webos_modules_init(1 0 0 QUALIFIER RC2)
webos_component(0 1 0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FindPkgConfig)

pkg_check_modules(GLIB2 REQUIRED glib-2.0)
include_directories(${GLIB2_INCLUDE_DIRS})

# GDBus, for biomd on the system bus; gio-unix for passing frames as memfds.
pkg_check_modules(GIO REQUIRED gio-2.0 gio-unix-2.0)
include_directories(${GIO_INCLUDE_DIRS})

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})

# Colour conversion and rotation of camera frames, and the FACED_DUMP_DIR
# frame dumps. The matcher itself lives in biomd.
pkg_check_modules(OPENCV REQUIRED opencv4)
include_directories(${OPENCV_INCLUDE_DIRS})

# camera-utils (com.webos.service.camera) ships no pkg-config file; its headers
# land in ${includedir}/camera and it provides these two libraries.
find_library(CAMERA_SHM_LIB NAMES camera_shared_memory REQUIRED)
find_library(LUNA_CLIENT_LIB NAMES luna_client REQUIRED)

webos_add_compiler_flags(ALL -Wall -Wextra)
webos_add_linker_options(ALL --no-undefined)

include_directories(src)
file(GLOB SOURCE_FILES src/*.cpp)

add_executable(luneos-faced ${SOURCE_FILES})
target_link_libraries(luneos-faced
    ${GIO_LDFLAGS}
    ${GLIB2_LDFLAGS}
    ${LUNASERVICE2_LDFLAGS}
    ${OPENCV_LDFLAGS}
    ${CAMERA_SHM_LIB}
    ${LUNA_CLIENT_LIB}
    pthread)

webos_build_daemon()

# Unit tests (not part of the image): cmake -DBUILD_TESTING=ON
option(BUILD_TESTING "Build the unit tests" OFF)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
webos_build_system_bus_files()
