# Composition cookbook {#cookbook}

[TOC]

The `compositions/` directory holds nine files: eight composition documents and the
meteorology series two of them read. Each section below says what one file demonstrates,
how to run it, and what the run produces. Every result shown was produced by running the
file: the documents built from this repository's plugins by
`docs/examples/cookbook_compositions` (`ctest -R docs.example.cookbook_compositions`) and by
`CompositionsTest`, the three FVQual demonstrations by `DemoTest`.

| File | Demonstrates | Plugins | Checked by |
| --- | --- | --- | --- |
| `source_to_recorder.json` | a direct connection | signal source, recorder | `CompositionsTest`, `docs.example.cookbook_compositions` |
| `source_gain_recorder_adapted.json` | a chain of adapted outputs from a standalone factory | signal source, basic adapters, gain transformer, recorder | the same |
| `combiner_roles.json` | providers connected by role to a multi-input | signal source, combiner, recorder | the same |
| `bound_arguments.json` | an argument bound to another component's output | signal source, gain transformer, recorder | the same |
| `interval-transfer.json` | the interval-transfer profile, with receipts | interval source and receiver | `docs.example.cookbook_compositions` |
| `demo_met.csv` | the forcing series of the two FVQual demonstrations | | `DemoTest` |
| `demo_a.json` | mesh and forcing providers feeding a solver live | mesh generator, time series provider, FVQual | `DemoTest` |
| `demo_b_meshgen.json` | a provider run on its own, its results recorded | mesh generator | `DemoTest`, `docs.example.cookbook_compositions` |
| `demo_b_open.json` | a provider served from a recorded run | mesh generator (opened), time series provider, FVQual | `DemoTest` |

## Running a composition

**In the test suite.** After a build, `ctest --test-dir build -R CompositionsTest` runs the
four testing documents and compares each recording with the expected rows.
`ctest --test-dir build -R docs.example.cookbook_compositions` runs the documents that need
only this repository's plugins, and prints the output shown on this page.

**In Composer.** Load the plugins with "Load Directory..." pointed at `build/components` (or
put that directory in `HYDROCOUPLE_COMPONENT_PATH`), open the document, and run it. Composer
sets each component's reference directory to the document's directory, so a recorder's
relative `path` lands beside the document. Copy a document to a directory of your own first:
the runs write files beside it.

**Headless, with Composer.** `HydroCoupleComposer --run <composition.json> --components
<plugin-directory>` runs a document without a window; `--components` may be repeated. This
is the host that carries out a document's `writers` and `run` blocks. The command is taken
from Composer's command-line parser; it was not run for this manual, because the Composer
build here is not built against the same HydroCoupleSDK.

**How the cookbook runner applies a document.** It does what a host does, which is also what
Composer does: it creates one instance per component block from the block's
`info.component_info_id`, gives the instance the block's `id` with `setInstanceId()`, sets
its reference directory (here, the runner's working directory,
`build/docs-examples/cookbook_compositions/`), applies the document with
`ModelInitializer::initializeFromFile()` with the SDK's adapter factory and the Basic
Adapters factory installed, steps a `TimeSteppedWorkflow` to Done, and finishes every
component. It then prints each component's final scalar outputs and anything a component
queued on `errors()`. It does not carry out `writers` or `run` blocks.

## source_to_recorder.json: a direct connection

The smallest coupled composition: a constant signal, `offset + amplitude` = 3 + 2 = 5, for
three steps, connected straight to a recorder.

\include{lineno} source_to_recorder.json

The run takes three workflow updates: the source reports Done on its third step, and the
recorder follows it.

```text
source_to_recorder.json
  workflow: 3 updates, Done
  source.signal = 5
  recorded_direct.csv:
step,value
1,5
2,5
3,5
```

## source_gain_recorder_adapted.json: an adapter chain

A ramp passes through two adapted outputs from the standalone Basic Adapters factory,
`negate` and then `clamp` to [−5, −3], before the gain transformer applies 2x + 1:

<pre class="mermaid">
flowchart LR
    S[Signal Source: ramp 2, 4, 6] --> N[negate: -2, -4, -6]
    N --> C[clamp to -5 ... -3: -3, -4, -5]
    C --> G[Gain Transformer: 2x + 1]
    G --> R[Recorder: -5, -7, -9]
</pre>

\include{lineno} source_gain_recorder_adapted.json

`adapted_outputs` is applied in order: each entry adapts the output of the one before it.
`factory` names the factory by its instance ID, and `arguments` are set before the adapted
output initializes. The host must install the factory (see
@ref ref_basicadapters "Basic Adapters"); with the SDK's factory alone, the document fails
with `no adapted output "negate" is offered for output "signal"`. Values that pass the clamp
unchanged prove nothing, so the clamp's bounds cut the first and last samples. Wrong values
here mean the standalone factory was not consulted, the steps were applied out of order, or
the chain was not refreshed.

```text
source_gain_recorder_adapted.json
  workflow: 3 updates, Done
  source.signal = 6
  gain.out = -9
  recorded_adapted.csv:
step,value
1,-5
2,-7
3,-9
```

## combiner_roles.json: providers by role

Two constant sources, 3 and 4, connected to the combiner's multi-input under the roles
`augend` and `addend`:

\include{lineno} combiner_roles.json

The connection's `to.role` names the role; without it, or with a role the combiner does not
declare, `ModelInitializer` refuses the connection (see @ref ref_combiner "Combiner").

```text
combiner_roles.json
  workflow: 2 updates, Done
  first.signal = 3
  second.signal = 4
  combiner.combined = 7
  recorded_combined.csv:
step,value
1,7
2,7
```

## bound_arguments.json: an argument bound to an output {#cookbook_bound}

The gain transformer's `bias` argument names the `signal` output of a second source,
`calibration`, with an `@from` binding instead of a value:

\include{lineno} bound_arguments.json

`ModelInitializer` orders the components so that a bound provider comes before its
consumer. For the binding it validates, prepares and updates `calibration` until it is
Done, finishes it, and initializes `bias` from its output: 99 + 1 = 100. Only then does the
gain transformer initialize, latching `bias` = 100. The run adds 100 to every sample of
`source`, which is 5:

```text
bound_arguments.json
  workflow: 2 updates, Done
  calibration.signal = 100
  source.signal = 5
  gain.out = 105
  errors() of calibration: illegal status transition Finished -> Validating (Helpers::isValidComponentStatusTransition rejects it)
  recorded_bound.csv:
step,value
1,105
2,105
```

The `errors()` line is a known defect, harmless to the result. `calibration` is Finished when
the workflow starts, but the time-stepped workflow validates and prepares every component it
manages, and the signal source's `validate()` moves to Validating from any state. The SDK
records the transition the interfaces' table rejects, and the workflow then runs
`calibration` a second time; `bias` was already latched, so the recording is unchanged.

## interval-transfer.json: conserved amounts with receipts

The interval-transfer reference pair (@ref interval_transfer "reference"): a source that
offers two intervals of water, heat and two nitrogen species, and a receiver that accepts
them and writes a receipt per interval.

\include{lineno} interval-transfer.json

Things to notice:

- `config` holds the configuration object itself, not a `values` array.
- Every receiver input is connected, by name, to the source output with the same ID; the
  receiver refuses a partial or adapted connection.
- `receipt_file` is relative, so it resolves against the receiver's reference directory;
  the receiver creates `out/`.
- `writers` (a CSV of the recorded outputs) and `run` (a run manifest) are host blocks:
  Composer carries them out, and the cookbook runner does not.

With the block IDs as instance IDs, as Composer assigns them, the transfer IDs are
`controlled-source:1` and `controlled-source:2`:

```text
interval-transfer.json
  workflow: 2 updates, Done
  controlled-source.epoch_julian_day = 2.44059e+06
  controlled-source.interval_start_s = 60
  controlled-source.interval_end_s = 90
  controlled-source.water_m3 = 3
  controlled-source.enthalpy_j = 150
  controlled-source.species.NH4_N = 0.3
  controlled-source.species.NO3_N = 0.15
  conservative-receiver.rejected_water_m3 = 0
  conservative-receiver.accepted.water_m3 = 3
  conservative-receiver.cumulative.water_m3 = 5
  conservative-receiver.accepted.enthalpy_j = 150
  conservative-receiver.cumulative.enthalpy_j = 250
  conservative-receiver.accepted.species.NH4_N = 0.3
  conservative-receiver.cumulative.species.NH4_N = 0.5
  conservative-receiver.accepted.species.NO3_N = 0.15
  conservative-receiver.cumulative.species.NO3_N = 0.25
  out/interval-receipts.csv:
instance_id,transfer_id,epoch_julian_day,start_s,end_s,offered_water_m3,accepted_water_m3,rejected_water_m3,scale,accepted_enthalpy_j,species.NH4_N_kg-N,species.NO3_N_kg-N
"conservative-receiver","controlled-source:1",2440587.5,0,60,2,2,0,1,100,0.20000000000000001,0.10000000000000001
"conservative-receiver","controlled-source:2",2440587.5,60,90,3,3,0,1,150,0.29999999999999999,0.14999999999999999
```

The cumulative outputs close the budget: 5 m3, 250 J, 0.5 kg-N of `NH4_N` and 0.25 kg-N of
`NO3_N`, with nothing rejected. (The runner prints with six significant digits; the
receipt carries 17.)

## demo_met.csv: the demonstrations' forcing

A one-series CSV, air temperature 5 (°C for FVQual) at the times 0, 0.5 and 1:

\verbinclude demo_met.csv

The first column holds numbers, so the times are taken as Julian days as they are. The
@ref ref_timeseriesprovider "time series provider" reads it into one output,
`air_temperature`. The two demonstrations name it by a relative path, which the provider
opens relative to the process's working directory: run them from the directory that holds
the file. `DemoTest` copies the documents and this file into
`build/test-artifacts/demos/demo_a/` and `demo_b/` and runs there.

## demo_a.json: providers feeding a solver live

The mesh generator meshes a 600 by 100 basin, the time series provider reads
`demo_met.csv`, and FVQual's solver (`fvqual.component`, from the FVQual repository) takes
both, in one composition:

\include{lineno} demo_a.json

The mesh reaches the solver through an `@from` binding to its `mesh` argument; the series
reaches it twice, through a binding to `met_air_temperature` (which enables meteorology)
and through a connection to its `air_temperature` input carrying the SDK's
`temporal_interpolation` adapter. `writers` and `run` ask the host to record results and a
run manifest.

`DemoTest.DemoARunsAllThreeProvidersLiveAndItsManifestReopens` runs it with a runner that
mirrors `fvqual run` and carries out the CSV writer and the manifest. Run against an FVQual
build of the same HydroCoupleSDK, the workflow reaches Done after three updates (FVQual's
geometry arguments give a three-hour run); the recorded run reopens without any model
library; every recorded air temperature is 5, the source's value; and the basin's last
recorded temperature is below 10 and above 0, which the test reads as proof that the met
binding enabled meteorology and the short run cooled the surface slightly. In the run for
this manual the last step's temperatures, over the basin's 104 cells, lay between 9.9535
and 9.9709.

## demo_b_meshgen.json: a provider on its own

The first half of demonstration B: the mesh generator alone, with the same arguments as in
Demo A.

\include{lineno} demo_b_meshgen.json

The cookbook runner serves the mesh, 28 nodes on the boundary and 26 triangles:

```text
demo_b_meshgen.json
  workflow: 1 updates, Done
  meshgen.mesh: 28 nodes, 26 faces
```

`DemoTest.DemoBRecordsTheMeshAndOpenModeServesItBack` runs it, writes the mesh to an HDF5
UGRID file (`meshgen_mesh.h5`) and records the manifest `meshgen_run.json`, the file the
second half opens. It checks that the stored mesh reopens as a polyhedral surface with the
generated face count.

## demo_b_open.json: a provider served from a recorded run

The second half of demonstration B: Demo A's document with one change, an `execution`
block on `meshgen`:

\dontinclude demo_b_open.json
\skip "id": "meshgen"
\until "execution"

With `"mode": "open"`, `ModelInitializer` does not run the mesh generator. It opens the
recorded run named in `results` (relative to the document) as an SDK
`ResultsModelComponent` and serves its recorded `mesh` output to the solver's binding in
the generator's place. `DemoTest.DemoAAndDemoBAgreeAndDifferOnlyInTheExecutionBlock`
checks both claims: the two documents are identical once the `execution` block is
removed, metadata included, which is why this file's `metadata` repeats Demo A's ID and
caption; and the two runs record byte-identical temperature histories.

## smoke.json: one component alone

Not in `compositions/`: `docs/examples/cookbook_compositions/smoke.json` is the
@ref ref_smoke "smoke component" on its own, the smallest document the runner applies:

\include{lineno} smoke.json

```text
smoke.json
  workflow: 1 updates, Done
  smoke.produced = 42
```
