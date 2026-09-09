cmake_minimum_required(VERSION 3.20)
project(chtest VERSION 0.2.0 LANGUAGES CXX)
include(GNUInstallDirs)
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()

option(CHTEST_BUILD_TESTS "Build tests and register them with CTest" ON)
option(CHTEST_BUILD_BENCHMARKS "Build reproducible performance probes" OFF)
option(CHTEST_ENABLE_ASAN "Enable AddressSanitizer for project executables" OFF)
option(CHTEST_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer for project executables" OFF)
option(CHTEST_ENABLE_TSAN "Enable ThreadSanitizer for project executables" OFF)
if(CHTEST_ENABLE_ASAN AND CHTEST_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
endif()
if((CHTEST_ENABLE_UBSAN OR CHTEST_ENABLE_TSAN) AND (MSVC OR WIN32))
    message(FATAL_ERROR "UBSan/TSan configurations require GCC or Clang on a supported non-Windows platform")
endif()

find_package(Threads REQUIRED)
add_library(chtest INTERFACE)
add_library(chtest::chtest ALIAS chtest)
target_include_directories(chtest INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_features(chtest INTERFACE cxx_std_17)
target_link_libraries(chtest INTERFACE Threads::Threads)

function(chtest_configure_target target)
    target_link_libraries(${target} PRIVATE chtest::chtest)
    set_target_properties(${target} PROPERTIES CXX_STANDARD ${CMAKE_CXX_STANDARD} CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(CHTEST_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address /Zi)
            target_link_options(${target} PRIVATE /INCREMENTAL:NO /DEBUG)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(CHTEST_ENABLE_UBSAN)
            target_compile_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=undefined -fno-sanitize-recover=undefined)
        endif()
        if(CHTEST_ENABLE_TSAN)
            target_compile_options(${target} PRIVATE -fsanitize=thread -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=thread)
        endif()
        if(CHTEST_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer -g)
            target_link_options(${target} PRIVATE -fsanitize=address)
            if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
                target_link_options(${target} PRIVATE -shared-libasan)
                execute_process(COMMAND ${CMAKE_CXX_COMPILER} --print-runtime-dir
                    OUTPUT_VARIABLE chtest_runtime_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
                file(GLOB chtest_asan_dlls "${chtest_runtime_dir}/clang_rt.asan_dynamic-*.dll")
                foreach(chtest_asan_dll IN LISTS chtest_asan_dlls)
                    add_custom_command(TARGET ${target} POST_BUILD
                        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                            "${chtest_asan_dll}" "$<TARGET_FILE_DIR:${target}>")
                endforeach()
            endif()
        endif()
    endif()
endfunction()

if(CHTEST_BUILD_TESTS)
    enable_testing()
    add_executable(chtest_tests
        test/main.cpp test/test_env.cpp test/vector_tests.cpp
        test/fixture_env_tests.cpp test/param_and_basic_tests.cpp
        test/tag_tests.cpp test/mock_tests.cpp test/threading_tests.cpp
        test/advanced_tests.cpp test/regression_cases.cpp)
    chtest_configure_target(chtest_tests)
    add_executable(chtest_regressions test/runner_regressions.cpp)
    chtest_configure_target(chtest_regressions)
    add_executable(chtest_failure_examples test/failure_examples.cpp)
    chtest_configure_target(chtest_failure_examples)
    foreach(target IN ITEMS property_tests assertion_contracts concurrency_stress allocation_tests)
        add_executable(chtest_${target} test/${target}.cpp)
        chtest_configure_target(chtest_${target})
    endforeach()

    add_test(NAME suite_serial COMMAND chtest_tests --quiet --no-color)
    add_test(NAME suite_parallel COMMAND chtest_tests --quiet --no-color --threads 4)
    add_test(NAME suite_repeat_shuffle COMMAND chtest_tests --quiet --no-color --threads 4 --repeat 3 --shuffle 42)
    add_test(NAME suite_unbuffered COMMAND chtest_tests --quiet --no-color --no-buffer)
    add_test(NAME suite_timing COMMAND chtest_tests --quiet --no-color --timings --timeout 5000)
    add_test(NAME runner_regressions COMMAND chtest_regressions)
    foreach(seed IN ITEMS 42 20260908 4294967295)
        add_test(NAME properties_${seed} COMMAND chtest_property_tests ${seed})
        set_tests_properties(properties_${seed} PROPERTIES LABELS "properties" TIMEOUT 60)
    endforeach()
    add_test(NAME assertion_contracts COMMAND chtest_assertion_contracts)
    add_test(NAME concurrency_stress COMMAND chtest_concurrency_stress)
    add_test(NAME allocation_contracts COMMAND chtest_allocation_tests)
    add_test(NAME installed_package COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/package-verification"
        "-DCXX_COMPILER=${CMAKE_CXX_COMPILER}" "-DGENERATOR=${CMAKE_GENERATOR}"
        "-DCONFIG=$<CONFIG>" "-DMAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DCXX_STANDARD=${CMAKE_CXX_STANDARD}" -P ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_install.cmake)
    set_tests_properties(assertion_contracts PROPERTIES LABELS "contracts" TIMEOUT 60)
    set_tests_properties(concurrency_stress PROPERTIES LABELS "stress" TIMEOUT 120)
    set_tests_properties(allocation_contracts PROPERTIES LABELS "memory" TIMEOUT 120)
    set_tests_properties(installed_package PROPERTIES LABELS "integration" TIMEOUT 120)
    add_test(NAME expected_failure_examples COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:chtest_failure_examples> -P ${CMAKE_CURRENT_SOURCE_DIR}/test/verify_failures.cmake)
    set_tests_properties(suite_serial suite_parallel suite_repeat_shuffle suite_unbuffered suite_timing
        runner_regressions expected_failure_examples PROPERTIES TIMEOUT 60)
endif()

if(CHTEST_BUILD_BENCHMARKS)
    add_executable(chtest_benchmarks test/benchmarks.cpp)
    chtest_configure_target(chtest_benchmarks)
endif()

include(CMakePackageConfigHelpers)
install(TARGETS chtest EXPORT chtestTargets)
install(FILES include/chtest.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT chtestTargets NAMESPACE chtest:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chtest)
configure_package_config_file(cmake/chtestConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/chtestConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chtest)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/chtestConfigVersion.cmake
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chtestConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/chtestConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/chtest)
