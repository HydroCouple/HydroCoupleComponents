# Mesh Generator {#ref_meshgenerator}

[TOC]

The mesh generator triangulates a boundary polygon with the SDK's `Tools::Triangulator`,
optionally merges the triangles into a quad-dominant mesh with `Tools::QuadMesher`, and
serves the result as a polyhedral-surface output. A solver adopts the mesh through an
argument binding in the same composition, or reads it later from a recorded run or a UGRID
file. Generation is a pure delegation to the two tools: `MeshGeneratorTest` builds the same
domain through the component and through direct calls to the tools and compares the meshes
node for node and face for face.

| | |
| --- | --- |
| Library | `hcc_meshgenerator`, built as `build/components/libhcc_meshgenerator.so`; links the SDK's HydroCoupleTools library |
| Component info ID | `hydrocouple.components.meshgenerator` |
| Caption, version, tags | Mesh Generator; 0.1.0; Provider, Mesh |
| Default instance ID | `hydrocouple.components.meshgenerator.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent`; output: `Spatial::IPolyhedralSurfaceComponentDataItem` |
| Source | `components/meshgenerator/meshgeneratorcomponent.cpp` |

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `boundary` | Float64 table, N rows by 2 columns (x, y) | the square (0, 0), (100, 0), (100, 100), (0, 100) | the model's length unit; the component does not interpret it | N >= 3; exactly 2 columns; counter-clockwise; first vertex not repeated | The outer ring to triangulate. Rows are on the dimension `vertex`, columns on `xy`. |
| `max_edge_length` | Float64 | 0 | the boundary's | any; values <= 0 turn it off | When greater than 0, boundary edges longer than this are split into equal pieces no longer than it, before triangulation. It does not limit interior edges. |
| `quad_quality` | Float64 | 0 | none | [0, 1] | 0 keeps the triangles. A larger value merges adjacent triangle pairs into quadrilaterals, best quality first; a pair whose quadrilateral quality falls below this threshold stays two triangles. |
| `output_path` | String, optional | empty | none | a writable file path, or empty | When set, the mesh is also written to this path as a UGRID NetCDF file. A relative path is relative to the process's working directory. File filter `UGRID NetCDF (*.nc)`. |

The defaults mesh a 100 by 100 square, so an unconfigured instance, which a host such as
Composer initializes only to learn its ports, is valid and runnable.

A `boundary` payload is a list of rows, each a list of two numbers:

```json
"boundary": { "values": [[0.0, 0.0], [600.0, 0.0], [600.0, 100.0], [0.0, 100.0]] }
```

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Output | `mesh` | none; shape `[]` | polyhedral surface (the generated mesh), located on faces | the mesh of the last successful update; no time stamp |

The output is geometry only. Its `shape()` is empty, it has no entity dimension, and
reading values from it fails with `The mesh output is geometry; read its meshView()`. A
consumer reads the surface through `polyhedralSurface()` and its `meshView()`; the SDK's
`Spatial::PolyhedralSurfaceArgument` does that when it is initialized from the output. The
output owns the mesh: a pointer taken from `polyhedralSurface()` stays valid until the
next update or the destruction of the component. Before the first successful update the
surface is null.

## Behavior

### initialize()

Accepts any arguments and creates the empty `mesh` output. Nothing is computed:
triangulation waits for `update()`, so initializing is cheap.

### validate()

Checks, and returns every problem found, in this order:

| Problem | Condition |
| --- | --- |
| `The boundary needs at least three vertices; it has <n>` | fewer than three rows |
| `The boundary must be an N x 2 table of x, y` | a column count other than 2 |
| `quad_quality must lie in [0, 1]; 0 keeps triangles` | `quad_quality` below 0 or above 1 |

The status is Invalid when there is a problem and Valid otherwise. The problems are
returned only; they are not queued on `errors()`, although the interface asks that every
returned message also be queued. Orientation, self-intersection and `max_edge_length` are
not checked, and the triangulator does not refuse them either: a check of four 10 by 10
squares, counter-clockwise, clockwise, with the first vertex repeated, and with two
vertices swapped so that the ring crosses itself, ran all four to Done with two faces each.
Give the ring counter-clockwise, as the triangulator expects, without a repeated vertex,
and check the ring before a run: the component does not.

### prepare()

Moves to Updated with the message `prepared`.

### update()

Does all the work in one step:

1. Reports Updating with the message `triangulating`.
2. Triangulates the boundary with `max_edge_length`. The triangulator's progress stages
   arrive as Updating status messages: `densifying`, `inserting vertices`, `inserting
   edges`, `removing holes`, `extracting`, `done`.
3. When `quad_quality` is above 0, merges with the quad mesher, whose stages follow:
   `indexing edges`, `scoring pairs`, `merging`, `copying triangles`, `done`.
4. When `output_path` is set, writes the UGRID file. The writer creates missing parent
   directories and overwrites an existing file. The file's mesh topology variable is named
   `triangulation`, the name the triangulator gives its meshes.
5. Replaces the served mesh and reports Done with the message `meshed`.

A failure at steps 2 to 4 moves to Failed with the tool's or the writer's message, and the
previous mesh, if any, stays served.

### finish()

Moves through Finishing to Finished. The mesh stays readable until the instance is
destroyed.

## Status messages

A run with `max_edge_length` 50 and `quad_quality` 0.6:

```text
[meshgenerator] status messages of a run with quad_quality 0.6
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Updating "triangulating"
    Updating "densifying"
    Updating "inserting vertices"
    Updating "inserting vertices"
    Updating "inserting edges"
    Updating "removing holes"
    Updating "extracting"
    Updating "done"
    Updating "indexing edges"
    Updating "scoring pairs"
    Updating "merging"
    Updating "copying triangles"
    Updating "done"
    Done "meshed"
    Finishing ""
    Finished ""
```

## Errors

```text
[meshgenerator] errors
  validate() of a 2 x 3 boundary and quad_quality 1.5
    The boundary needs at least three vertices; it has 2
    The boundary must be an N x 2 table of x, y
    quad_quality must lie in [0, 1]; 0 keeps triangles
  errors() after that validate()
  a collinear boundary
    triangulation produced no triangles
  output_path below a regular file
    Writing 'recorder_run.csv/mesh.nc' failed: nc_create(impl.path.string().c_str(), NC_NETCDF4 | NC_CLOBBER, &impl.ncid): Permission denied
  reading values from the mesh output
    The mesh output is geometry; read its meshView()
```

| Message | Cause | Fix |
| --- | --- | --- |
| `The boundary needs at least three vertices; it has <n>` | `boundary` has fewer than three rows. The status is Invalid. | Give at least three vertices. |
| `The boundary must be an N x 2 table of x, y` | `boundary` rows do not have two values. | Write each row as `[x, y]`. |
| `quad_quality must lie in [0, 1]; 0 keeps triangles` | `quad_quality` is out of range. | Use a value from 0 to 1. |
| `triangulation produced no triangles` | The triangulator kept no triangle, for example for three collinear vertices. The status is Failed. | Give a ring that encloses an area. |
| `triangulation failed: <reason>` and `triangulation failed: constraint (<x1>, <y1>)-(<x2>, <y2>) could not be recovered` | The triangulation library (MeshKit) could not insert a vertex or recover a boundary edge. The status is Failed. | Simplify the ring: remove near-duplicate vertices and crossings. |
| ``Writing '<path>' failed: <NetCDF message>`` | The UGRID file could not be created: the path is below a regular file, or not writable. | Choose a writable path. |
| `output_path is set but this SDK build has no NetCDF support; rebuild with USE_NETCDF or clear output_path` | The SDK the plugin was built against has no NetCDF (`USE_NETCDF` off). | Rebuild the SDK with `-DUSE_NETCDF=ON` and this repository against it, or clear `output_path`. |
| `The mesh output is geometry; read its meshView()` | A consumer read `mesh` through `getValuesInto()`. | Read `polyhedralSurface()->meshView()`, or bind a `PolyhedralSurfaceArgument` to the output. |
| `The mesh output is produced by the run` | Code tried to write `mesh`. | Set the arguments instead. |

## Example compositions

`compositions/demo_b_meshgen.json` runs the generator alone over a 600 by 100 rectangle
with `max_edge_length` 50:

\include{lineno} demo_b_meshgen.json

The cookbook runner reports the mesh it served: every boundary edge is split into 50-unit
pieces, and the triangulator needs no interior vertex for this rectangle:

```text
demo_b_meshgen.json
  workflow: 1 updates, Done
  meshgen.mesh: 28 nodes, 26 faces
```

The `run` block asks a host to record a run manifest; the cookbook runner does not.

To hand the mesh to a solver in the same composition, bind the solver's mesh argument to
the output. This is the document `MeshGeneratorTest` applies to a consumer component named
`solver` and the generator named `meshgen`:

\dontinclude test_meshgenerator.cpp
\skip ASSERT_TRUE(initializer.initializeFromJsonText(R"({
\until })", message)) << message;

`ModelInitializer` runs `meshgen` to Finished before `solver` initializes, then
initializes the solver's `PolyhedralSurfaceArgument` from the output.
`compositions/demo_a.json` does the same for the FVQual solver; see the
@ref cookbook "cookbook".

## Tests

| Test | What it checks |
| --- | --- |
| `MeshGeneratorTest.TheGeneratedMeshIsTheToolsOwnNodeForNode` | the component's mesh equals the triangulator's, node coordinates and face connectivity, on an asymmetric quadrilateral |
| `MeshGeneratorTest.QuadMergingFollowsTheQualityArgument` | with `quad_quality` 0.6 the faces equal the quad mesher's, and fewer than the triangles |
| `MeshGeneratorTest.DefaultsProduceAValidMeshOutOfTheBox` | an unconfigured instance validates, runs and serves faces |
| `MeshGeneratorTest.ATwoVertexBoundaryFailsValidationByName` | a two-vertex boundary is reported by name |
| `MeshGeneratorTest.AnFromBindingHandsTheMeshToAConsumersArgument` | a `@from` binding gives a consumer's `PolyhedralSurfaceArgument` the mesh, with no file |
| `MeshGeneratorTest.TheWrittenUgridFileHoldsTheProducedMesh` | the UGRID file reads back with the same nodes and faces; built only with an SDK that has NetCDF |
| `DemoTest.*` | the generator live (Demo A), standalone and recorded, then opened from the record (Demo B); needs FVQual's plugin |
| `docs.example.cookbook_compositions` | `demo_b_meshgen.json` serves 28 nodes and 26 faces |
| `docs.example.reference_messages` | the status messages and errors above |
