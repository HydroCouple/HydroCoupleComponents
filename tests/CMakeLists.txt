# SPDX-License-Identifier: MIT
find_package(GTest REQUIRED)

add_executable(HydroCoupleComponentsTests
    test_smokeplugin.cpp
    test_meshgenerator.cpp
    test_timeseriesprovider.cpp
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
    HCC_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

add_dependencies(HydroCoupleComponentsTests hcc_smoke hcc_meshgenerator hcc_timeseriesprovider)

include(GoogleTest)
gtest_discover_tests(HydroCoupleComponentsTests)
