# HydroCoupleComponents

Provider components for the HydroCouple ecosystem: processes that are
components in their own right — mesh generators, timeseries providers —
whose outputs initialize and parameterize other components, either bound
live through a composition's `@from` argument bindings or run standalone
with their results recorded for later use.

Each component builds as a loadable plugin library publishing itself
through HydroCoupleSDK's `componentabi.h` convention, so
HydroCoupleComposer, the headless runner, and the Python loader all pick
it up the same way.

## Building

```sh
cmake -S . -B build -DCMAKE_PREFIX_PATH=<hydrocouplesdk-install-prefix>
cmake --build build
ctest --test-dir build
```

Without an installed SDK, pass `-DHCC_SDK_SOURCE_DIR=<path>` to build it
in-tree.

## Components

Providers:

- `hcc_meshgenerator` — MeshGeneratorComponent: triangulates a boundary
  with the SDK Triangulator, optionally merged to quad-dominant by the
  QuadMesher, and provides the mesh live or as a UGRID file.
- `hcc_timeseriesprovider` — TimeSeriesProviderComponent: CSV, CE-QUAL-W2
  `.npt`, SWMM-style `.dat` and USGS NWIS (cached for offline reruns), one
  output per series.

Testing family (deterministic components for exercising compositions):

- `hcc_signalsource`, `hcc_gaintransformer`, `hcc_recorder`, `hcc_combiner`
  and `hcc_basicadapters`, a standalone adapted-output factory.
- `hcc_smoke` — the scaffold's proof plugin (test fixture, not a tool).

Interval transfer: `hcc_intervalsource` and `hcc_intervalreceiver`, the
reference pair described below.

Planned (see `plans/`): NetCDF CF series in TimeSeriesProviderComponent
(refused with a message today), TerrainSamplerComponent.

## License

MIT — see `LICENSE`.

## Custom interval-transfer reference

The [developer guide](docs/INTERVAL_TRANSFER_COMPONENTS.md) describes the
executable source/receiver pair, configured species metadata, typed water/mass/heat
ports, receipt accounting and custom-component conformance tests. The worked
composition is `compositions/interval-transfer.json`.
