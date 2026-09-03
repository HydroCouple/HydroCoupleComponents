# SPDX-License-Identifier: MIT
hcc_add_component(hcc_meshgenerator meshgeneratorcomponent.cpp)
target_link_libraries(hcc_meshgenerator PRIVATE HydroCoupleSDK::HydroCoupleTools)
