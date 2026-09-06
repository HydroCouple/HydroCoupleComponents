# SPDX-License-Identifier: MIT
find_package(GTest REQUIRED)

add_executable(HydroCoupleComponentsTests
    test_smokeplugin.cpp
    test_meshgenerator.cpp
    test_timeseriesprovider.cpp
    test_signalchain.cpp
    test_combiner.cpp
    test_basicadapters.cpp
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
    HCC_COMPONENTS_DIR="${CMAKE_BINARY_DIR}/components"
    HCC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    HCC_ARTIFACTS_DIR="${CMAKE_BINARY_DIR}/test-artifacts")

add_dependencies(HydroCoupleComponentsTests hcc_smoke hcc_meshgenerator
    hcc_timeseriesprovider hcc_signalsource hcc_gaintransformer hcc_recorder
    hcc_combiner hcc_basicadapters)

include(GoogleTest)
gtest_discover_tests(HydroCoupleComponentsTests)
