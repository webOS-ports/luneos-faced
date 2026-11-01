# SPDX-License-Identifier: GPL-2.0-only
# Copyright (c) 2026 Herman van Hazendonk <github.com@herrie.org>
#
# Unit tests: src/identify_post.cpp against a fake libluna-service2
# (tests/fake_ls2.cpp).

pkg_check_modules(GTEST REQUIRED gtest_main)

add_executable(faced-tests
    test_identify_post.cpp
    fake_ls2.cpp
    ${CMAKE_SOURCE_DIR}/src/identify_post.cpp)
target_include_directories(faced-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR} ${GTEST_INCLUDE_DIRS})
target_compile_options(faced-tests PRIVATE -Wall -Wextra)
target_link_libraries(faced-tests ${GTEST_LDFLAGS} ${GLIB2_LDFLAGS} pthread)

add_test(NAME faced-tests COMMAND faced-tests)
