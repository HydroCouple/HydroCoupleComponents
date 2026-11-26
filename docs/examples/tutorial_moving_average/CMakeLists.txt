# SPDX-License-Identifier: MIT
# The tutorial: a component library, packaged and loaded by a small host.

# [plugin]
# The component library. A shipped component gets the same build properties
# from one line in its own directory, hcc_add_component(<target> <sources>).
add_library(hcc_docs_movingaverage MODULE movingaveragecomponent.cpp)
target_link_libraries(hcc_docs_movingaverage
    PRIVATE HydroCoupleSDK::HydroCoupleSDK)
set_target_properties(hcc_docs_movingaverage PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/docs-examples/components")
# [plugin]

# [install]
# Packaging: the shipped plugins' destination, under an install component
# that a plain "cmake --install" leaves out, so the tutorial never ships.
install(TARGETS hcc_docs_movingaverage
    LIBRARY DESTINATION lib/hydrocouple/components
    COMPONENT hcc_docs_examples
    EXCLUDE_FROM_ALL)
# [install]

# [tests]
# First install into a staging prefix, then let the host search the staged
# directory and the build's shipped plugins, load, run and unload.
set(stage "${CMAKE_BINARY_DIR}/docs-examples/tutorial_moving_average/stage")
add_test(NAME docs.example.tutorial_moving_average.install
         COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
                 --component hcc_docs_examples --prefix "${stage}")
set_tests_properties(docs.example.tutorial_moving_average.install PROPERTIES
    FIXTURES_SETUP tutorial_stage LABELS docs-example)

hcc_add_docs_example(tutorial_moving_average
    SOURCES main.cpp
    ARGS "${stage}/lib/hydrocouple/components" "${CMAKE_BINARY_DIR}/components")
add_dependencies(docs_example_tutorial_moving_average hcc_docs_movingaverage)
set_tests_properties(docs.example.tutorial_moving_average PROPERTIES
    FIXTURES_REQUIRED tutorial_stage)
# [tests]
