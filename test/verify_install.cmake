function(checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE status OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 90)
    if(NOT status STREQUAL "0")
        message(FATAL_ERROR "Command failed (${status}): ${ARGV}\n${out}\n${err}")
    endif()
endfunction()

# Configure a header-only producer so the package must stand on its exported
# dependencies. A custom layout catches hardcoded include/lib assumptions.
foreach(layout IN ITEMS default custom)
    set(producer "${BINARY_DIR}/${layout}/producer")
    set(prefix "${BINARY_DIR}/${layout}/prefix")
    set(consumer "${BINARY_DIR}/${layout}/consumer")
    set(dirs)
    if(layout STREQUAL "custom")
        list(APPEND dirs -DCMAKE_INSTALL_INCLUDEDIR=headers/chtest -DCMAKE_INSTALL_LIBDIR=lib/chtest)
    endif()
    checked("${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${producer}" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
        -DCHTEST_BUILD_TESTS=OFF -DCHTEST_BUILD_BENCHMARKS=OFF ${dirs})
    checked("${CMAKE_COMMAND}" --install "${producer}" --prefix "${prefix}" --config "${CONFIG}")
    if(layout STREQUAL "custom")
        set(package_dir "${prefix}/lib/chtest/cmake/chtest")
    else()
        # Let find_package search the standard layout on the current platform.
        set(package_dir "")
    endif()
    checked("${CMAKE_COMMAND}" -S "${SOURCE_DIR}/test/consumer" -B "${consumer}" -G "${GENERATOR}"
        "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}" "-DCMAKE_MAKE_PROGRAM=${MAKE_PROGRAM}"
        "-DCMAKE_PREFIX_PATH=${prefix}" "-Dchtest_DIR=${package_dir}"
        "-DCMAKE_CXX_STANDARD=${CXX_STANDARD}" "-DCMAKE_BUILD_TYPE=${CONFIG}")
    checked("${CMAKE_COMMAND}" --build "${consumer}" --config "${CONFIG}" --parallel 2)
    checked("${CMAKE_CTEST_COMMAND}" --test-dir "${consumer}" -C "${CONFIG}" --output-on-failure)
endforeach()
message(STATUS "Default and custom-layout packages compile and run in two translation units")
