# SPDX-License-Identifier: MIT
find_package(GTest REQUIRED)

add_executable(HydroCoupleComponentsTests
    test_smokeplugin.cpp
    test_meshgenerator.cpp
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
    HCC_COMPONENTS_DIR="${CMAKE_BINARY_DIR}/components")

add_dependencies(HydroCoupleComponentsTests hcc_smoke hcc_meshgenerator)

include(GoogleTest)
gtest_discover_tests(HydroCoupleComponentsTests)
