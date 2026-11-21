# Getting started {#getting_started}

[TOC]

This page builds the plugins, runs their tests and runs a first composition. Every
command here was run against HydroCoupleSDK 2.0 on macOS; the repository's CI runs the
same build on Linux and macOS.

## Requirements

| Requirement | Used for |
| --- | --- |
| CMake 3.24 or newer and a C++20 compiler | the build |
| HydroCoupleSDK 2.0, installed or as a source checkout | every plugin links it; the mesh generator also links the SDK's HydroCoupleTools library |
| nlohmann_json | found through the SDK's package configuration |
| GoogleTest | the test suite (`HCC_BUILD_TESTS`, on by default) |
| An SDK built with NetCDF (`USE_NETCDF`) | only the mesh generator's optional UGRID file output |

HydroCoupleSDK is not on a package registry yet. The CI workflow
(`.github/workflows/build_and_unit_test.yml`) builds and installs it from source with
vcpkg first, then builds this repository against that installation.

## Build

From the repository root:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<hydrocouplesdk-install-prefix>
cmake --build build
ctest --test-dir build
```

When the SDK's own dependencies were installed by vcpkg, add that vcpkg tree to
`CMAKE_PREFIX_PATH` after the SDK prefix, separated by a semicolon, as the CI workflow
does. Without an installed SDK, pass `-DHCC_SDK_SOURCE_DIR=<path-to-HydroCoupleSDK>` and
CMake builds the SDK in-tree.

Each plugin is a CMake `MODULE` library written to `build/components`. On both Linux and
macOS the files are named `lib<target>.so`, for example `build/components/libhcc_smoke.so`.
Installing copies them to `lib/hydrocouple/components` below the prefix:

```sh
cmake --install build --prefix <prefix>
```

## Run the tests

`ctest --test-dir build` runs the `HydroCoupleComponentsTests` executable: 50 tests in
eight suites, or 49 when the SDK was built without NetCDF, which drops the UGRID file
test. Each suite loads the plugins it needs from `build/components` with `dlopen`, the way
a host does, and drives them through the HydroCouple interfaces. Run one suite by name
with `-R`:

```sh
ctest --test-dir build -R SignalChainTest --output-on-failure
```

Tests that write files put them in `build/test-artifacts`, so a run leaves the recorded
CSV files and interval receipts there for review.

A second executable, `HydroCoupleComponentsDemos`, couples the mesh generator and the time
series provider with the FVQual solver plugin. CMake builds it only when FVQual's built
plugin is found in `FVQUAL_COMPONENTS_DIR` (by default `../FVQual/build/components`);
otherwise the configure step prints that the demo suite is not built.

## The plugins

| Library | Component info ID | Caption | Family |
| --- | --- | --- | --- |
| `hcc_meshgenerator` | `hydrocouple.components.meshgenerator` | Mesh Generator | @ref providers "Providers" |
| `hcc_timeseriesprovider` | `hydrocouple.components.timeseriesprovider` | Time Series Provider | @ref providers "Providers" |
| `hcc_signalsource` | `hydrocouple.components.signal_source` | Signal Source | @ref testing_components "Testing" |
| `hcc_gaintransformer` | `hydrocouple.components.gain` | Gain Transformer | @ref testing_components "Testing" |
| `hcc_recorder` | `hydrocouple.components.recorder` | Recorder | @ref testing_components "Testing" |
| `hcc_combiner` | `hydrocouple.components.combiner` | Combiner | @ref testing_components "Testing" |
| `hcc_basicadapters` | `hydrocouple.components.adapters.basic` | Basic Adapters | @ref testing_components "Testing" |
| `hcc_intervalsource` | `org.hydrocouple.openswmm.test.interval-source` | Controllable interval source | @ref interval_transfer "Interval transfer" |
| `hcc_intervalreceiver` | `org.hydrocouple.openswmm.test.sink` | Conservative interval receiver | @ref interval_transfer "Interval transfer" |
| `hcc_smoke` | `hydrocouple.components.smoke` | Smoke Component | @ref concepts "Scaffold" |

A composition names a component by its info ID (`info.component_info_id`). Each library
exports exactly two C functions and nothing else; on macOS:

```sh
nm -gU build/components/libhcc_smoke.so
```

```
00000000000012b0 T _hydrocouple_component_abi_v1
00000000000012bc T _hydrocouple_component_info_v1
```

@ref concepts "Concepts" explains what the two functions do and how a host uses them.

## Load the plugins in a host

A host loads the libraries from directories it searches. HydroCoupleComposer searches a
`components` directory beside its executable, `lib/hydrocouple/components` in its
installation prefix, every directory listed in the `HYDROCOUPLE_COMPONENT_PATH`
environment variable, and directories added in its component settings: "Load
Directory..." pointed at `build/components` loads a build tree directly.
The compositions in `compositions/` are the manual smoke deck: open them in Composer once
the plugins are loaded.

## A first composition

`compositions/source_to_recorder.json` connects a constant signal to a recorder:

\include{lineno} source_to_recorder.json

The signal source emits `offset + amplitude` = 5 for three steps, and the recorder writes
one row per step. `CompositionsTest.SourceToRecorderCompositionRunsAndRecordsEveryStep`
applies this document through the SDK's `ModelInitializer`, runs it with a
`TimeSteppedWorkflow`, and checks the recorded file, `recorded_direct.csv`:

```
step,value
1,5
2,5
3,5
```

The recorder's `path` is relative, so it resolves against the component's reference
directory: the composition's own directory in a host, and `build/test-artifacts` in the
test, which sets that directory on every instance it creates.

## Next steps

- @ref concepts "Concepts": the loading convention, the component lifecycle, and the two
  ways a provider's results reach another component.
- @ref providers "Provider components", @ref testing_components "Testing components" and
  @ref interval_transfer "Interval-transfer components": arguments, ports and behavior of
  each plugin.
