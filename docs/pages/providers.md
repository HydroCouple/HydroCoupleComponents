# Provider components {#providers}

[TOC]

Providers are components in their own right whose outputs initialize and parameterize
other components: a mesh for a solver, a forcing series for a model. Both providers here
leave the numbers exactly as their sources make them, so a consumer sees what the SDK's
mesh tools or the source file produced and nothing else.

| Component | What it provides | Page |
| --- | --- | --- |
| Mesh Generator | a triangulated, optionally quad-dominant, mesh of a boundary polygon, as a polyhedral-surface output and optionally a UGRID file | @subpage ref_meshgenerator "Mesh Generator" |
| Time Series Provider | one time-series output per series of a CSV, CE-QUAL-W2 `.npt` or SWMM-style `.dat` file, or a USGS NWIS answer | @subpage ref_timeseriesprovider "Time Series Provider" |

## How a provider's results reach a consumer

A provider's results reach a consumer in one of two ways, described in full on
@ref concepts "Concepts":

- **Live**, in one composition: a consumer's argument names the provider's output with an
  `@from` binding, and `ModelInitializer` runs the provider to Finished first and hands the
  output to the argument; or the provider's output feeds an input through a connection.
- **Standalone**, in two runs: the provider runs on its own and its results are kept, in a
  UGRID file (the mesh generator's `output_path`) or in a run manifest a host records; a
  later composition opens them with `"execution": { "mode": "open" }`.

Both providers do their expensive work at different points. The mesh generator
triangulates in `update()`, so initializing it to learn its ports costs nothing. The time
series provider reads its source while it initializes, because its outputs are the
source's series. `compositions/demo_a.json`, `demo_b_meshgen.json` and `demo_b_open.json`
show both pathways with the FVQual solver; see the @ref cookbook "cookbook".
