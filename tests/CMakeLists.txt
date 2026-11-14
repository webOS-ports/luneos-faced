# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Unit tests, in two binaries so the first needs no opencv:
#  faced-tests
#   - src/identify_post.cpp against a fake libluna-service2 (fake_ls2.cpp)
#   - src/biomd_face.cpp against a fake biomd on a private bus (fake_biomd.cpp);
#     GTestDBus starts a dbus-daemon, which has to be on PATH
#  faced-orientation-tests
#   - src/frame_orientation.cpp

pkg_check_modules(GTEST REQUIRED gtest_main)

add_executable(faced-tests
    test_identify_post.cpp
    test_biomd_face.cpp
    test_camera_provider.cpp
    fake_ls2.cpp
    fake_biomd.cpp
    ${CMAKE_SOURCE_DIR}/src/identify_post.cpp
    ${CMAKE_SOURCE_DIR}/src/biomd_face.cpp
    ${CMAKE_SOURCE_DIR}/src/camera_provider.cpp)
target_include_directories(faced-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR} ${GTEST_INCLUDE_DIRS})
target_compile_options(faced-tests PRIVATE -Wall -Wextra)
target_link_libraries(faced-tests ${GTEST_LDFLAGS} ${GIO_LDFLAGS} ${GLIB2_LDFLAGS} pthread)
add_test(NAME faced-tests COMMAND faced-tests)

add_executable(faced-orientation-tests
    test_frame_orientation.cpp
    ${CMAKE_SOURCE_DIR}/src/frame_orientation.cpp)
target_include_directories(faced-orientation-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${GTEST_INCLUDE_DIRS})
target_compile_options(faced-orientation-tests PRIVATE -Wall -Wextra)
target_link_libraries(faced-orientation-tests ${GTEST_LDFLAGS} ${GLIB2_LDFLAGS}
    ${OPENCV_LDFLAGS} pthread)
add_test(NAME faced-orientation-tests COMMAND faced-orientation-tests)
