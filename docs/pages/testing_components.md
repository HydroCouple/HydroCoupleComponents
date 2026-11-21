# Testing components {#testing_components}

[TOC]

The testing family is a set of small components whose results follow exactly from their
arguments, so a composition built from them has a known answer. Hosts and end-to-end
tests use them to check wiring, argument bindings, multi-input roles and adapter chains
with exact values. None of them models anything physical, and all their quantities are
unitless.

| Component | Library | Component info ID | Ports |
| --- | --- | --- | --- |
| Signal Source | `hcc_signalsource` | `hydrocouple.components.signal_source` | output `signal` |
| Gain Transformer | `hcc_gaintransformer` | `hydrocouple.components.gain` | input `in`, output `out` |
| Recorder | `hcc_recorder` | `hydrocouple.components.recorder` | input `in` |
| Combiner | `hcc_combiner` | `hydrocouple.components.combiner` | multi-input `operands`, output `combined` |
| Basic Adapters | `hcc_basicadapters` | `hydrocouple.components.adapters.basic` | adapted outputs `negate`, `clamp` |

All five are version 1.0.0 and tagged Test. Every port is a rank-1 Float64 item of length
1, with two exceptions: the combiner's `operands` holds one value per connected provider,
and the adapters accept a rank-1 Float64 output of any length. Each model component's
default instance ID is its info ID followed by `.instance`.

## Signal Source

Emits one sample of a waveform per update, for `steps` updates, then reports Done.

| Argument | Type | Default |
| --- | --- | --- |
| `waveform` | String: `sine`, `square`, `ramp` or `constant` | `sine` |
| `amplitude` | Float64 | 1 |
| `frequency` | Float64, cycles per step | 1 |
| `offset` | Float64 | 0 |
| `steps` | Integer, at least 1 | 10 |

At step *n* = 1 ... `steps`, with phase *p* = `frequency` * *n*:

| Waveform | `signal` |
| --- | --- |
| `sine` | `offset + amplitude * sin(2 pi p)` |
| `square` | `offset + amplitude` where `sin(2 pi p)` >= 0, otherwise `offset - amplitude` |
| `ramp` | `offset + amplitude * (p - floor(p))` |
| `constant` | `offset + amplitude` |

An unknown waveform, or `steps` below 1, makes `initialize()` fail with a message naming
the allowed values. `prepare()` resets the step count.

## Gain Transformer

Applies `out = gain * in + bias` each update.

| Argument | Type | Default |
| --- | --- | --- |
| `gain` | Float64 | 1 |
| `bias` | Float64 | 0 |

It reports Done when the component providing `in` is Done or Finished, and Updated
before that; with nothing connected it is Done after one update.

## Recorder

Writes what arrives at `in` to a CSV file, one `step,value` row per update, so a run
leaves a file a person can open.

| Argument | Type | Default |
| --- | --- | --- |
| `path` | String, required | empty |

An empty `path` makes `initialize()` fail. A relative path resolves against the
component's reference directory, which a host sets to the composition document's
directory. `prepare()` creates or truncates the file and writes the header `step,value`;
`finish()` closes it. If the file cannot be opened, `prepare()` reports an error and sets
the status to Failed. Like the gain transformer, the recorder reports Done once its
provider is Done or Finished.

## Combiner

Combines the values of every provider connected to its multi-input `operands` with one
operation per update.

| Argument | Type | Default |
| --- | --- | --- |
| `operation` | String: `sum`, `product` or `mean` | `sum` |

`operands` is an SDK `AbstractMultiInput` with two provider roles, `augend` and `addend`,
both required. A connection must name a role: in a composition, the `to.role` field. A
connection without a role, or with a role the combiner does not declare, is refused with
a message listing the roles that do exist. Each provider must be a rank-1 Float64 output.
The combiner reports Done once every connected provider is Done or Finished.

## Basic Adapters

An adapted-output factory, not a model component: it publishes an
`IAdaptedOutputFactoryComponentInfo` and does not answer as an `IModelComponentInfo`, so a
host keeps it among its adapter factories and never places it in a composition as a
component. Its offerings are
deliberately different from the SDK's own factory (`hydrocouple.sdk.adapters`), so a
composition that names them proves the library was consulted.

| Adapted output | Arguments | Result, element by element |
| --- | --- | --- |
| `negate` | none | `-x` |
| `clamp` | `minimum` (default -1e30), `maximum` (default 1e30) | `x` limited to [`minimum`, `maximum`] |

Both are offered for any rank-1 Float64 output, including another adapted output, so they
chain. The factory instance's ID is the info ID itself, because a composition's
`adapted_outputs` entries name the factory by that ID. As the interface requires, a
created adapted output is already registered with its provider; the registration does not
own it, so the caller unregisters it before destroying it.

## Compositions

The `compositions/` directory holds four documents built from this family.
`CompositionsTest` applies each one through `ModelInitializer`, with the Basic Adapters
factory installed beside the SDK's, runs it with a `TimeSteppedWorkflow` until Done, and
compares the recorder's file with the expected rows. Relative recorder paths resolve
against `build/test-artifacts` there.

### A direct connection

`source_to_recorder.json` is on @ref getting_started "Getting started".

### An adapter chain

`source_gain_recorder_adapted.json` passes a ramp through two adapters before the gain
transformer:

<pre class="mermaid">
flowchart LR
    S[Signal Source: ramp 2, 4, 6] --> N[negate: -2, -4, -6]
    N --> C[clamp to -5 ... -3: -3, -4, -5]
    C --> G[Gain Transformer: 2x + 1]
    G --> R[Recorder: -5, -7, -9]
</pre>

\include{lineno} source_gain_recorder_adapted.json

Recorded in `recorded_adapted.csv`:

```
step,value
1,-5
2,-7
3,-9
```

### Providers by role

`combiner_roles.json` sums two constant sources, 3 and 4, connected under the `augend` and
`addend` roles:

\include{lineno} combiner_roles.json

Recorded in `recorded_combined.csv`:

```
step,value
1,7
2,7
```

### An argument bound to an output

`bound_arguments.json` binds the gain transformer's `bias` argument to the `signal` output
of a second source, `calibration`. `ModelInitializer` runs `calibration` to Finished
first, so `bias` is its last sample, 100, before the gain transformer initializes. The
run then adds 100 to every sample of `source`, which is 5:

\include{lineno} bound_arguments.json

Recorded in `recorded_bound.csv`:

```
step,value
1,105
2,105
```

## Driving the components directly

The same components can be driven without a composition document. This is
`SignalChainTest.GainTransformerScalesItsInputEachStep`: it wires a constant source to the
gain transformer across the two libraries, the way `ModelInitializer` wires a connection,
and checks that the transformer keeps to its provider's clock:

\dontinclude test_signalchain.cpp
\skip GainTransformerScalesItsInputEachStep
\until EXPECT_EQ(gain->status(), ComponentStatus::Done);
\line }
