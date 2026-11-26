# SPDX-License-Identifier: MIT
# HydroCoupleComponents — provider components for the HydroCouple ecosystem.
#
# Each component builds as a loadable plugin library publishing itself
# through the SDK's componentabi.h convention (the stamped
# hydrocouple_component_abi_v1 / hydrocouple_component_info_v1 pair), so
# HydroCoupleComposer's registry, the headless runner, and the Python
# loader all pick them up the same way.

cmake_minimum_required(VERSION 3.24)

project(HydroCoupleComponents VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HCC_BUILD_TESTS "Build the test suite" ON)

# ── HydroCoupleSDK (required) ─────────────────────────────────────────────
#
# Preferred: an installed SDK found via CMAKE_PREFIX_PATH.
# Fallback:  point HCC_SDK_SOURCE_DIR at a source checkout to build it
#            in-tree (the SDK is not on a package registry yet).
set(HCC_SDK_SOURCE_DIR "" CACHE PATH
    "HydroCoupleSDK source checkout to build in-tree when no installed package is found")

find_package(HydroCoupleSDK 2.0 CONFIG)

if(NOT HydroCoupleSDK_FOUND)
    if(HCC_SDK_SOURCE_DIR AND EXISTS "${HCC_SDK_SOURCE_DIR}/CMakeLists.txt")
        message(STATUS "HydroCoupleComponents: building HydroCoupleSDK in-tree from ${HCC_SDK_SOURCE_DIR}")
        add_subdirectory("${HCC_SDK_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_hydrocouplesdk")
    else()
        message(FATAL_ERROR
            "HydroCoupleSDK 2.0 was not found.\n"
            "  Either install it and pass -DCMAKE_PREFIX_PATH=<sdk-install-prefix>,\n"
            "  or pass -DHCC_SDK_SOURCE_DIR=<path-to-HydroCoupleSDK> to build it in-tree.")
    endif()
endif()

# Both discovery paths must yield the same target names.
if(NOT TARGET HydroCoupleSDK::HydroCoupleSDK AND TARGET HydroCoupleSDK)
    add_library(HydroCoupleSDK::HydroCoupleSDK ALIAS HydroCoupleSDK)
endif()
if(NOT TARGET HydroCoupleSDK::HydroCoupleTools AND TARGET HydroCoupleTools)
    add_library(HydroCoupleSDK::HydroCoupleTools ALIAS HydroCoupleTools)
endif()

# ── Shared plugin conventions ─────────────────────────────────────────────
#
# Hidden visibility everywhere: the ABI header marks the two entry points
# default-visibility itself, and the interface headers force their own RTTI
# visible, so nothing else needs to leak.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

#! Declares one component plugin library with the house conventions.
function(hcc_add_component name)
    add_library(${name} MODULE ${ARGN})
    target_link_libraries(${name} PRIVATE HydroCoupleSDK::HydroCoupleSDK)
    set_target_properties(${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/components")
    install(TARGETS ${name} LIBRARY DESTINATION lib/hydrocouple/components)
endfunction()

add_subdirectory(components/smoke)
add_subdirectory(components/meshgenerator)
add_subdirectory(components/timeseriesprovider)
add_subdirectory(components/signalsource)
add_subdirectory(components/gaintransformer)
add_subdirectory(components/recorder)
add_subdirectory(components/combiner)
add_subdirectory(components/basicadapters)
add_subdirectory(components/intervalreceiver)

if(HCC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # The manual's compiled examples, run as docs.example.* tests.
    add_subdirectory(docs/examples)
endif()
