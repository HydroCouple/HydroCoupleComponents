# SPDX-License-Identifier: MIT
find_package(GTest REQUIRED)

add_executable(HydroCoupleComponentsTests
    test_smokeplugin.cpp
    test_intervalreceiver.cpp
    test_meshgenerator.cpp
    test_timeseriesprovider.cpp
    test_signalchain.cpp
    test_combiner.cpp
    test_basicadapters.cpp
    test_compositions.cpp
)

target_link_libraries(HydroCoupleComponentsTests
    PRIVATE
        HydroCoupleSDK::HydroCoupleSDK
        GTest::gtest
        GTest::gtest_main
        HydroCoupleSDK::HydroCoupleTools
        ${CMAKE_DL_LIBS}
)

target_compile_definitions(HydroCoupleComponentsTests PRIVATE
    HCC_SOURCE_LIBRARY="$<TARGET_FILE:hcc_intervalsource>"
    HCC_RECEIVER_LIBRARY="$<TARGET_FILE:hcc_intervalreceiver>"
    HCC_COMPONENTS_DIR="${CMAKE_BINARY_DIR}/components"
    HCC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    HCC_ARTIFACTS_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
    HCC_COMPOSITIONS_DIR="${CMAKE_SOURCE_DIR}/compositions")

add_dependencies(HydroCoupleComponentsTests hcc_intervalsource hcc_intervalreceiver hcc_smoke hcc_meshgenerator
    hcc_timeseriesprovider hcc_signalsource hcc_gaintransformer hcc_recorder
    hcc_combiner hcc_basicadapters)

include(GoogleTest)
gtest_discover_tests(HydroCoupleComponentsTests)

# ── ARGGEN P5 demos ──────────────────────────────────────────────────────
# The end-to-end demonstrations couple this repo's providers with FVQual's
# plugin from its own repository build. The target exists only when that
# build is present, so a checkout without FVQual still configures.
set(FVQUAL_COMPONENTS_DIR "${CMAKE_SOURCE_DIR}/../FVQual/build/components"
    CACHE PATH "Directory holding FVQual's built component plugin")

if(EXISTS "${FVQUAL_COMPONENTS_DIR}/libfvqual-component.so" OR
   EXISTS "${FVQUAL_COMPONENTS_DIR}/libfvqual-component.dylib")
    add_executable(HydroCoupleComponentsDemos test_demos.cpp)
    target_link_libraries(HydroCoupleComponentsDemos
        PRIVATE
            HydroCoupleSDK::HydroCoupleSDK
            GTest::gtest
            GTest::gtest_main
            ${CMAKE_DL_LIBS}
    )
    target_compile_definitions(HydroCoupleComponentsDemos PRIVATE
        HCC_COMPONENTS_DIR="${CMAKE_BINARY_DIR}/components"
        HCC_ARTIFACTS_DIR="${CMAKE_BINARY_DIR}/test-artifacts"
        HCC_COMPOSITIONS_DIR="${CMAKE_SOURCE_DIR}/compositions"
        FVQUAL_COMPONENTS_DIR="${FVQUAL_COMPONENTS_DIR}")
    add_dependencies(HydroCoupleComponentsDemos hcc_meshgenerator
        hcc_timeseriesprovider)
    gtest_discover_tests(HydroCoupleComponentsDemos)
else()
    message(STATUS
        "FVQual plugin not found at ${FVQUAL_COMPONENTS_DIR}; "
        "the ARGGEN demo suite is not built")
endif()
