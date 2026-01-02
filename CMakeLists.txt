cmake_minimum_required(VERSION 3.20)

project(chtest LANGUAGES CXX)

option(CHTEST_BUILD_TESTS "Build chtest tests" ON)

add_library(chtest INTERFACE)

target_include_directories(chtest
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/test
)

target_compile_features(chtest INTERFACE cxx_std_20)

if (CHTEST_BUILD_TESTS)
    add_executable(chtest_tests
        test/main.cpp
        test/test_env.cpp
        test/vector_tests.cpp
        test/fixture_env_tests.cpp
        test/param_and_basic_tests.cpp
        test/tag_tests.cpp
        test/mock_tests.cpp
        test/threading_tests.cpp
        test/advanced_tests.cpp
    )

    target_link_libraries(chtest_tests PRIVATE chtest)
    
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(chtest_tests PRIVATE -O3 -Wall -Wextra -Wpedantic -std=c++20)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(chtest_tests PRIVATE -O3 -Wall -Wextra -Wpedantic -std=c++20)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(chtest_tests PRIVATE /O2 /W4 /std:c++20)
    endif()

    

endif()
