# SPDX-License-Identifier: MIT
# docs/examples/CMakeLists.txt: the manual's compiled examples
#
# Each directory here holds one example that a manual page shows with
# \include, \snippet or \dontinclude. Examples build with the test suite
# (HCC_BUILD_TESTS) and run as the ctest test docs.example.<directory>
# (label docs-example), so a page cannot show code or output that no longer
# compiles or runs.
#
#   <directory>/CMakeLists.txt  optional: the example declares its own targets
#                               and tests (the tutorial builds a plugin)
#   <directory>/main.cpp        otherwise: every *.cpp in the directory builds
#                               one host program, run without arguments
#
# A program reads its inputs through DOCS_EXAMPLE_SOURCE_DIR, finds the built
# plugins in HCC_COMPONENTS_DIR and the composition documents in
# HCC_COMPOSITIONS_DIR, and writes into its working directory,
# build/docs-examples/<directory>/, which stays for review after the run.
# Examples are never installed with the shipped plugins.

# An installed HydroCoupleSDK library carries no runtime search path for its
# own shared dependencies (NetCDF, HDF5, SQLite, yaml-cpp and zlib when a
# dynamic vcpkg triplet built them), so the executable that loads it must
# supply one. The test executable gets it by linking GoogleTest from the same
# vcpkg tree; the example hosts name that tree's library directory.
set(hcc_docs_example_rpath "")
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    set(hcc_docs_example_rpath
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
endif()

#! Builds one example host program and registers its test.
#! hcc_add_docs_example(<name> SOURCES <file>... [DIRECTORY <dir>]
#!                      [ARGS <argument>...])
#! DIRECTORY is the example's source directory (default: the current one).
function(hcc_add_docs_example name)
    cmake_parse_arguments(PARSE_ARGV 1 example "" "DIRECTORY" "SOURCES;ARGS")
    if(NOT example_DIRECTORY)
        set(example_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
    endif()
    set(target "docs_example_${name}")
    add_executable(${target} ${example_SOURCES})
    target_link_libraries(${target} PRIVATE
        HydroCoupleSDK::HydroCoupleSDK ${CMAKE_DL_LIBS})
    target_compile_definitions(${target} PRIVATE
        DOCS_EXAMPLE_SOURCE_DIR="${example_DIRECTORY}"
        HCC_COMPONENTS_DIR="${CMAKE_BINARY_DIR}/components"
        HCC_COMPOSITIONS_DIR="${CMAKE_SOURCE_DIR}/compositions")
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH "${hcc_docs_example_rpath}")
    # The shipped plugins an example loads must be built before it runs.
    add_dependencies(${target} hcc_smoke hcc_meshgenerator
        hcc_timeseriesprovider hcc_signalsource hcc_gaintransformer
        hcc_recorder hcc_combiner hcc_basicadapters hcc_intervalsource
        hcc_intervalreceiver)

    set(output "${CMAKE_BINARY_DIR}/docs-examples/${name}")
    file(MAKE_DIRECTORY "${output}")
    add_test(NAME docs.example.${name}
             COMMAND ${target} ${example_ARGS}
             WORKING_DIRECTORY "${output}")
    set_tests_properties(docs.example.${name} PROPERTIES
        LABELS docs-example TIMEOUT 300)
endfunction()

file(GLOB example_dirs LIST_DIRECTORIES true CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*")

foreach(dir IN LISTS example_dirs)
    if(NOT IS_DIRECTORY "${dir}")
        continue()
    endif()
    get_filename_component(name "${dir}" NAME)
    if(EXISTS "${dir}/CMakeLists.txt")
        add_subdirectory("${dir}")
    elseif(EXISTS "${dir}/main.cpp")
        file(GLOB sources CONFIGURE_DEPENDS "${dir}/*.cpp")
        hcc_add_docs_example(${name} SOURCES ${sources} DIRECTORY "${dir}")
    endif()
endforeach()
