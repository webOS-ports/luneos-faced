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

pkg_check_modules(LUNASERVICE2 REQUIRED luna-service2)
include_directories(${LUNASERVICE2_INCLUDE_DIRS})

pkg_check_modules(OPENCV REQUIRED opencv4)
include_directories(${OPENCV_INCLUDE_DIRS})

pkg_check_modules(OPENSSL REQUIRED libcrypto)
include_directories(${OPENSSL_INCLUDE_DIRS})

# camera-utils (com.webos.service.camera) ships no pkg-config file; its headers
# land in ${includedir}/camera and it provides these two libraries.
find_library(CAMERA_SHM_LIB NAMES camera_shared_memory REQUIRED)
find_library(LUNA_CLIENT_LIB NAMES luna_client REQUIRED)

# libfart likewise has no .pc; headers are in ${includedir}/fart.
find_library(FART_LIB NAMES fart REQUIRED)

# libtensorflow-lite.so, which libfart pulls in, references abseil's logging
# internals without linking them itself. Anything built with -Wl,--no-undefined
# has to supply them. The proper fix belongs in the tensorflow-lite recipe; until
# it lands, name them here.
find_library(ABSL_LOG_MESSAGE_LIB NAMES absl_log_internal_message REQUIRED)
find_library(ABSL_LOG_CHECK_OP_LIB NAMES absl_log_internal_check_op REQUIRED)
find_library(ABSL_LOG_NULLGUARD_LIB NAMES absl_log_internal_nullguard REQUIRED)

webos_add_compiler_flags(ALL -Wall -Wextra)
webos_add_linker_options(ALL --no-undefined)

include_directories(src)
file(GLOB SOURCE_FILES src/*.cpp)

add_executable(luneos-faced ${SOURCE_FILES})
target_link_libraries(luneos-faced
    ${GLIB2_LDFLAGS}
    ${LUNASERVICE2_LDFLAGS}
    ${OPENCV_LDFLAGS}
    ${OPENSSL_LDFLAGS}
    ${CAMERA_SHM_LIB}
    ${LUNA_CLIENT_LIB}
    ${FART_LIB}
    # --no-as-needed: the only references to these are inside
    # libtensorflow-lite.so, and GNU ld does not treat undefined symbols in a
    # shared library as a reason to keep a following --as-needed library, so
    # they would be dropped and the link would fail anyway.
    -Wl,--no-as-needed
    ${ABSL_LOG_MESSAGE_LIB}
    ${ABSL_LOG_CHECK_OP_LIB}
    ${ABSL_LOG_NULLGUARD_LIB}
    -Wl,--as-needed
    pthread)

webos_build_daemon()
webos_build_system_bus_files()
