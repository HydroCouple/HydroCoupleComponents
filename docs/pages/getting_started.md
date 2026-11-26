# Getting started {#getting_started}

[TOC]

This page builds the plugins, runs their tests and runs a first composition. Every
command here was run against HydroCoupleSDK 2.0 (`2.0.0-beta.1`) on macOS; the repository's
CI runs the same build on Linux and macOS.

## Requirements

| Requirement | Used for |
| --- | --- |
| CMake 3.24 or newer and a C++20 compiler | the build |
| HydroCoupleSDK 2.0, installed or as a source checkout | every plugin links it; the mesh generator also links the SDK's HydroCoupleTools library, so the SDK must be built with `BUILD_TOOLS=ON` |
| nlohmann_json | found through the SDK's package configuration |
| GoogleTest | the test suite (`HCC_BUILD_TESTS`, on by default) |
| An SDK built with NetCDF (`USE_NETCDF`) | only the mesh generator's optional UGRID file output and its test |

HydroCoupleSDK is not on a package registry yet. The CI workflow
(`.github/workflows/build_and_unit_test.yml`) builds and installs it from source with
vcpkg first, then builds this repository against that installation.

## Build the SDK

The SDK's own build is described in the
[HydroCouple SDK guide](https://www.hydrocouple.org/HydroCoupleSDK/). The configuration
used for this manual, with a vcpkg tree whose dependencies were already installed:

```sh
cmake -S HydroCoupleSDK -B build-sdk -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_INSTALL=OFF -DVCPKG_INSTALLED_DIR=<vcpkg_installed> \
  -DVCPKG_TARGET_TRIPLET=arm64-osx-dynamic \
  -DHYDROCOUPLE_INCLUDE_DIR=<HydroCouple>/include \
  -DUSE_GDAL=OFF -DUSE_NETCDF=ON -DUSE_HDF5=ON -DUSE_GEOPACKAGE=ON -DUSE_OPENMP=OFF \
  -DBUILD_TOOLS=ON -DBUILD_TESTS=OFF -DBUILD_EXAMPLES=OFF
cmake --build build-sdk --parallel 4
cmake --install build-sdk --prefix <absolute-sdk-prefix>
```

Give `--prefix` an absolute path: the installed package configuration records it.

## Build this repository

From the repository root:

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<hydrocouplesdk-install-prefix>
cmake --build build
ctest --test-dir build
```

When the SDK's dependencies were installed by vcpkg, also point this build at that vcpkg
tree: either pass the same `CMAKE_TOOLCHAIN_FILE`, `VCPKG_INSTALLED_DIR`,
`VCPKG_TARGET_TRIPLET` and `-DVCPKG_MANIFEST_INSTALL=OFF` as above, or add the triplet
directory to `CMAKE_PREFIX_PATH` after the SDK prefix, separated by a semicolon, as the CI
workflow does. Without an installed SDK, pass `-DHCC_SDK_SOURCE_DIR=<path-to-HydroCoupleSDK>`
and CMake builds the SDK in-tree.

| CMake option | Default | Effect |
| --- | --- | --- |
| `HCC_BUILD_TESTS` | `ON` | builds the test suite and the manual's compiled examples |
| `HCC_SDK_SOURCE_DIR` | empty | an SDK checkout to build in-tree when no installed SDK is found |
| `FVQUAL_COMPONENTS_DIR` | `../FVQual/build/components` | where the FVQual plugin is looked for; the demo suite is built only when it is found |

Each plugin is a CMake `MODULE` library written to `build/components`. On both Linux and
macOS the files are named `lib<target>.so`, for example `build/components/libhcc_smoke.so`.
Installing copies them to `lib/hydrocouple/components` below the prefix:

```sh
cmake --install build --prefix <prefix>
```

The manual's examples are not installed.

## Run the tests

`ctest --test-dir build` runs, without FVQual's plugin, 54 tests:

| Executable or test | Tests | What it covers |
| --- | --- | --- |
| `HydroCoupleComponentsTests` | 50 in eight suites (49 when the SDK has no NetCDF) | every plugin, loaded with `dlopen` and driven through the HydroCouple interfaces |
| `docs.example.tutorial_moving_average.install`, `docs.example.tutorial_moving_average` | 2 | @ref tutorial_component "the tutorial": install the example plugin, then load and run it |
| `docs.example.reference_messages` | 1 | the status and error messages listed in the @ref component_reference "component reference" |
| `docs.example.cookbook_compositions` | 1 | the compositions of the @ref cookbook "cookbook" that need only these plugins |
| `HydroCoupleComponentsDemos` | 3, when FVQual's plugin is found | the mesh generator and the time series provider feeding the FVQual solver |

Run one suite by name with `-R`, the examples by their label with `-L`:

```sh
ctest --test-dir build -R SignalChainTest --output-on-failure
ctest --test-dir build -L docs-example --output-on-failure
```

Tests that write files put them in `build/test-artifacts`, and the examples in
`build/docs-examples/<example>/`, so a run leaves the recorded CSV files, receipts and
meshes there for review.

The demo suite links against FVQual's plugin from its own repository build. That plugin
must be built against the same HydroCoupleSDK as this repository; an FVQual build from
another SDK fails to load with `Symbol not found`, and ctest then reports the three demo
tests as skipped (see @ref troubleshooting "Troubleshooting").

## The plugins

| Library | Component info ID | Caption | Reference |
| --- | --- | --- | --- |
| `hcc_meshgenerator` | `hydrocouple.components.meshgenerator` | Mesh Generator | @ref ref_meshgenerator "Mesh Generator" |
| `hcc_timeseriesprovider` | `hydrocouple.components.timeseriesprovider` | Time Series Provider | @ref ref_timeseriesprovider "Time Series Provider" |
| `hcc_signalsource` | `hydrocouple.components.signal_source` | Signal Source | @ref ref_signalsource "Signal Source" |
| `hcc_gaintransformer` | `hydrocouple.components.gain` | Gain Transformer | @ref ref_gaintransformer "Gain Transformer" |
| `hcc_recorder` | `hydrocouple.components.recorder` | Recorder | @ref ref_recorder "Recorder" |
| `hcc_combiner` | `hydrocouple.components.combiner` | Combiner | @ref ref_combiner "Combiner" |
| `hcc_basicadapters` | `hydrocouple.components.adapters.basic` | Basic Adapters | @ref ref_basicadapters "Basic Adapters" |
| `hcc_intervalsource` | `org.hydrocouple.openswmm.test.interval-source` | Controllable interval source | @ref interval_transfer "Interval transfer" |
| `hcc_intervalreceiver` | `org.hydrocouple.openswmm.test.sink` | Conservative interval receiver | @ref interval_transfer "Interval transfer" |
| `hcc_smoke` | `hydrocouple.components.smoke` | Smoke Component | @ref ref_smoke "Smoke Component" |

A composition names a component by its info ID (`info.component_info_id`). Each library
exports exactly two C functions and nothing else; on macOS:

```sh
nm -gU build/components/libhcc_smoke.so
```

```text
0000000000001250 T _hydrocouple_component_abi_v1
000000000000125c T _hydrocouple_component_info_v1
```

@ref concepts "Concepts" explains what the two functions do and how a host uses them.

## Load the plugins in a host

A host loads the libraries from directories it searches. HydroCoupleComposer searches, on
macOS, its bundle's `PlugIns/HydroCouple`; a `components` directory beside its executable;
`lib/hydrocouple/components` in its installation prefix; every directory listed in the
`HYDROCOUPLE_COMPONENT_PATH` environment variable; and directories added in its component
settings: "Load Directory..." pointed at `build/components` loads a build tree directly.
`HydroCoupleComposer --components <directory>` adds a directory from the command line. The
@ref tutorial_component "tutorial" writes a host's loading code from scratch.

## A first composition

`compositions/source_to_recorder.json` connects a constant signal to a recorder:

\include{lineno} source_to_recorder.json

The signal source emits `offset + amplitude` = 5 for three steps, and the recorder writes
one row per step. `CompositionsTest.SourceToRecorderCompositionRunsAndRecordsEveryStep`
applies this document through the SDK's `ModelInitializer`, runs it with a
`TimeSteppedWorkflow`, and checks the recorded file, `recorded_direct.csv`:

```text
step,value
1,5
2,5
3,5
```

The recorder's `path` is relative, so it resolves against the component's reference
directory: the composition's own directory in Composer, and `build/test-artifacts` in the
test, which sets that directory on every instance it creates. To run it in Composer, copy
the document to a directory of your own, load `build/components` and open the copy.

## Next steps

- @ref concepts "Concepts": the loading convention, the component lifecycle, arguments and
  exchange items, the workflow, and the two ways a provider's results reach another
  component.
- @ref tutorial_component "Tutorial": write, build, package and load a component of your
  own.
- @ref component_reference "Component reference": arguments, ports, behavior, messages and
  tests of each plugin.
- @ref cookbook "Composition cookbook": every document in `compositions/`, run.
- @ref troubleshooting "Troubleshooting": load failures, stamp mismatches, refused
  connections and arguments.
