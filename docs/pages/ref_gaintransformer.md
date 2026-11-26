# Gain Transformer {#ref_gaintransformer}

[TOC]

The gain transformer applies `out = gain * in + bias` to its input each update. It is the
testing family's transformer: the component a test or a demonstration puts between a
source and a recorder to prove that values cross a connection, an adapter chain or an
argument binding.

| | |
| --- | --- |
| Library | `hcc_gaintransformer`, built as `build/components/libhcc_gaintransformer.so` |
| Component info ID | `hydrocouple.components.gain` |
| Caption, version, tags | Gain Transformer; 1.0.0; Test, Transformer |
| Default instance ID | `hydrocouple.components.gain.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent` |
| Source | `components/gaintransformer/gaintransformercomponent.cpp` |

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `gain` | Float64 | 1 | none | any | Multiplies the input. |
| `bias` | Float64 | 0 | none | any | Added after the multiplication. |

Both are rank-1 items of length 1 on the dimension `scalar`, read at element 0 when the
component initializes. Either can be bound to another component's output with `@from`; the
bound value is the provider's output after it ran to Finished (see the example below).

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Input | `in` | "Signal", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the provider's value at this update |
| Output | `out` | "Transformed", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the value of the last update; no time stamp |

`in` is an SDK `Input1DDouble`. It accepts any provider whose data kind is Float64 and
whose rank is 1, of any length and any quantity, and reads only the provider's first value.
The connection does not compare units.

## Behavior

### initialize()

Latches `gain` and `bias`, creates `in` and `out` (both 0), and reaches Initialized.
Initialization cannot fail on these arguments' values.

### validate()

Moves to Valid. A missing provider is not a problem.

### prepare()

Moves to Updated with the message `prepared`.

### update()

1. Pulls the provider's first value into `in` (`applyInputValues()`). When there is no
   provider, or the read fails, `in` keeps its previous value.
2. Writes `gain * in + bias` to `out`.
3. Reports Done with the message `stepped` when the provider's component is Done or
   Finished, or when `in` has no provider or the provider belongs to no component;
   otherwise Updated with the same message.

Following the provider's clock this way, the transformer runs exactly as many steps as its
source. Unconnected, it is Done after one update.

### finish()

Moves through Finishing to Finished.

## Status messages

A run behind a two-step source, from `initialize()` on:

```text
[gain] status messages of a run behind a two-step source
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Updating ""
    Updated "stepped"
    Updating ""
    Done "stepped"
    Finishing ""
    Finished ""
```

(The listener was connected after `initialize()`, so the two SDK initialization messages,
`Initializing component` and `Component initialized successfully`, are not shown.)

## Errors

The component reports no errors of its own. An argument payload of the wrong type, or with
no values, fails the way the @ref ref_signalsource "signal source" arguments do. A provider
that is not Float64 or not rank 1 is refused at connection time by the SDK input, with
`Provider data kind mismatch` or `Provider is not one-dimensional` (see
@ref troubleshooting "Troubleshooting").

## Example compositions

`compositions/bound_arguments.json` binds `bias` to the final output of a second source:

\dontinclude bound_arguments.json
\skip "id": "gain"
\until } },

The cookbook's @ref cookbook_bound "argument binding" section walks through the run: `bias`
is 100, the input is 5, and the recording is 105 at each step.
`compositions/source_gain_recorder_adapted.json` sets `gain` 2 and `bias` 1 behind an
adapter chain.

Driven directly, without a composition, as `SignalChainTest` wires it:

\dontinclude test_signalchain.cpp
\skip GainTransformerScalesItsInputEachStep
\until EXPECT_EQ(gain->status(), ComponentStatus::Done);
\line }

## Tests

| Test | What it checks |
| --- | --- |
| `SignalChainTest.GainTransformerScalesItsInputEachStep` | 10 × 3 − 5 = 25, Updated after the first step and Done with its source |
| `CompositionsTest.AdaptedCompositionNegatesAndClampsThroughTheChain` | the transformer behind a negate and clamp chain records −5, −7, −9 |
| `CompositionsTest.BoundArgumentCompositionRunsItsProviderToFinishedFirst` | `bias` bound with `@from` records 105, 105 |
| `docs.example.reference_messages` | the status messages above |
