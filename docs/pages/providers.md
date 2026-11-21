# Provider components {#providers}

[TOC]

Providers are components in their own right whose outputs initialize and parameterize
other components: a mesh for a solver, a forcing series for a model. Both providers here
leave the numbers exactly as their sources make them, so a consumer sees what the SDK's
mesh tools or the source file produced and nothing else.

## Mesh Generator

| | |
| --- | --- |
| Library | `hcc_meshgenerator` |
| Component info ID | `hydrocouple.components.meshgenerator` |
| Default instance ID | `hydrocouple.components.meshgenerator.instance` |
| Version, tags | 0.1.0; Provider, Mesh |
| Source | `components/meshgenerator/meshgeneratorcomponent.cpp` |

The mesh generator triangulates a boundary polygon with the SDK's
`Tools::Triangulator`, optionally merges the triangles into a quad-dominant mesh with
`Tools::QuadMesher`, and serves the result as a polyhedral-surface output. Generation is
a pure delegation to those two tools: `MeshGeneratorTest` builds the same domain through
the component and through direct calls to the tools and compares the meshes node for node
and face for face.

### Arguments

| ID | Type | Default | Meaning |
| --- | --- | --- | --- |
| `boundary` | Float64 table, N rows by 2 columns (x, y) | the square (0, 0), (100, 0), (100, 100), (0, 100) | The outer ring. The SDK triangulator expects it counter-clockwise, without repeating the first vertex. |
| `max_edge_length` | Float64 scalar | 0 | When greater than 0, boundary edges longer than this are split before triangulation. |
| `quad_quality` | Float64 scalar in [0, 1] | 0 | 0 keeps the triangles. A larger value merges adjacent triangle pairs into quadrilaterals, best quality first; pairs whose quadrilateral quality falls below this threshold stay triangles. |
| `output_path` | String scalar, optional | empty | When set, the mesh is also written to this path as a UGRID NetCDF file. |

The coordinate, length and quality arguments are unitless.

### Output

| ID | Type | Contents |
| --- | --- | --- |
| `mesh` | polyhedral-surface data item (`IPolyhedralSurfaceComponentDataItem`) | The generated mesh, located on faces. The output is geometry only: its `shape()` is empty and reading values from it fails with a message pointing to the mesh view. |

A consumer adopts the mesh with an SDK `PolyhedralSurfaceArgument`, normally through an
argument binding. This is the composition `MeshGeneratorTest` applies to a consumer
component named `solver` and the generator named `meshgen`:

\dontinclude test_meshgenerator.cpp
\skip ASSERT_TRUE(initializer.initializeFromJsonText(R"({
\until })", message)) << message;

### Behavior

- `validate()` reports, by name, a boundary with fewer than three vertices, a boundary
  that is not N by 2, and a `quad_quality` outside [0, 1].
- `update()` does all the work in one step and reports Done with the message `meshed`.
  While it runs, the triangulator's and quad mesher's progress stages arrive as Updating
  status messages, so a host watching the component's status sees what it is doing.
- A tool failure, or a failure writing `output_path`, sets the status to Failed with the
  tool's message.
- `output_path` needs an SDK built with NetCDF. Without it, setting the argument makes the
  run fail with a message saying to rebuild the SDK with NetCDF or clear the argument. An
  unnamed mesh is written as `mesh2d`.

## Time Series Provider

| | |
| --- | --- |
| Library | `hcc_timeseriesprovider` |
| Component info ID | `hydrocouple.components.timeseriesprovider` |
| Default instance ID | `hydrocouple.components.timeseriesprovider.instance` |
| Version, tags | 0.1.0; Provider, Time Series |
| Source | `components/timeseriesprovider/timeseriesprovidercomponent.cpp` |

The time series provider reads one source and exposes one time-series output per series
it finds, ready for an exchange connection or an argument binding. Times are Julian days
throughout, the SDK's convention; calendar text is converted once, here.

### Arguments

| ID | Type | Default | Meaning |
| --- | --- | --- | --- |
| `source` | String scalar | empty | A file path, or an `http` or `https` URL. Empty is a valid provider with no series. |
| `format` | String scalar, one of `auto`, `csv`, `w2`, `swmm`, `nwis` | `auto` | `auto` chooses the reader from the file extension or the URL. |
| `cache` | String scalar, optional | empty | For web sources: where a successful fetch is saved, and what a failed fetch falls back to. |

### Outputs

One `TimeSeriesOutputDouble` per series, with the series name as its ID and a unitless
quantity of the same name. The source is read while the component initializes, because
its outputs are the source's series and a host initializes a component to learn its
outputs. `update()` changes nothing and reports Done.

### Sources and formats

| Source | `auto` chooses | Reader |
| --- | --- | --- |
| `*.csv` | `csv` | The first column is time and every other column is one series. A first line whose time field reads as neither a number nor a date is a header that names the series; without one they are named `<file stem>_<column>`. Blank lines and lines starting with `#` are skipped, and every row must have the same number of values. |
| `*.npt` | `w2` | CE-QUAL-W2 input deck: a title line, a header line naming the columns, then whitespace-separated rows of Julian day and values. Blank lines and lines starting with `$` or `#` are skipped. |
| `*.dat` | `swmm` | SWMM-style external time series: `date time value` rows, where a `time value` row reuses the previous row's date. Lines starting with `;` are comments. One series, named after the file stem. |
| URL containing `usgs.gov` or `/nwis/` | `nwis` | USGS NWIS JSON (instantaneous or daily values). Each `value.timeSeries` entry becomes a series named `<siteCode>_<parameterCode>`. |
| `*.nc` | | Refused: NetCDF CF time series are not implemented yet. The message suggests converting to CSV. |

A time is either a number, taken as a Julian day, or calendar text in one of two forms:
ISO, `2020-01-31 06:30` or `2020-01-31T06:30:00`, and US, `01/31/2020 06:30`. The time of
day is optional in both. NWIS time stamps are read as given: their time zone offsets are
not applied, so series from two zones are not silently shifted against each other.

A file with any other extension fails with a message listing the extensions tried and
asking for an explicit `format`. The `nwis` format reads web sources only; a URL that is
not recognized as NWIS needs `format` set to `nwis`.

### Web sources and the cache

The provider has no network code of its own. It fetches through the SDK's process-wide URI
resolver, which the host installs: Composer installs an HTTP resolver, and the tests
install a stub. When the fetch succeeds and `cache` is set, the answer is written to that
file. When the fetch fails, the provider reads `cache` instead, so a rerun without a
network serves the numbers the original run fetched. With neither a fetch nor a cache,
initialization fails naming both. To refresh, initialize again with the network back.

### Examples

The CSV fixture `tests/fixtures/timeseries/flows.csv`:

\verbinclude flows.csv

`TimeSeriesProviderTest.CsvColumnsBecomeOutputsWithParsedDatetimes` reads it into two
outputs, `flow` and `stage`, with the calendar times converted to Julian days:

\dontinclude test_timeseriesprovider.cpp
\skip CsvColumnsBecomeOutputsWithParsedDatetimes
\until }

The SWMM-style fixture `rain.dat` carries its date forward from row to row:

\verbinclude rain.dat

\dontinclude test_timeseriesprovider.cpp
\skip ASwmmDatCarriesTheDateForward
\until }

A consumer's `TimeSeriesArgumentDouble` binds to one of the outputs, here the `TAIR`
column of the CE-QUAL-W2 fixture `met.npt`:

\dontinclude test_timeseriesprovider.cpp
\skip const nlohmann::json document = {
\until fixture("met.npt")
