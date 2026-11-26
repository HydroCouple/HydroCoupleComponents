# Signal Source {#ref_signalsource}

[TOC]

The signal source emits one sample of a waveform per update, for a configured number of
steps, then reports Done. Its output follows exactly from its arguments, so a composition
that starts from it has a known answer. It is the clock of most testing compositions: the
gain transformer, the recorder and the combiner report Done when their providers do.

| | |
| --- | --- |
| Library | `hcc_signalsource`, built as `build/components/libhcc_signalsource.so` |
| Component info ID | `hydrocouple.components.signal_source` |
| Caption, version, tags | Signal Source; 1.0.0; Test, Source |
| Default instance ID | `hydrocouple.components.signal_source.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent` |
| Source | `components/signalsource/signalsourcecomponent.cpp` |

## Arguments

Every argument is a rank-1 item of length 1 on the dimension `scalar`. The component reads
element 0; a payload with more values is accepted and the rest are ignored.

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `waveform` | String | `sine` | none | `sine`, `square`, `ramp`, `constant` (case-sensitive) | Chooses the formula below. The argument carries a Quality listing the four names, so a host can offer them as choices. |
| `amplitude` | Float64 | 1 | none | any | Scales the waveform. |
| `frequency` | Float64 | 1 | cycles per step | any | Sets the phase *p* = `frequency` × *n* at step *n*. |
| `offset` | Float64 | 0 | none | any | Added to every sample. |
| `steps` | Integer | 10 | steps | at least 1 | The run length: the component reports Done on step `steps`. |

At step *n* = 1 … `steps`, with *p* = `frequency` × *n*:

| Waveform | `signal` |
| --- | --- |
| `sine` | `offset + amplitude * sin(2 pi p)` |
| `square` | `offset + amplitude` where `sin(2 pi p)` >= 0, otherwise `offset - amplitude` |
| `ramp` | `offset + amplitude * (p - floor(p))` |
| `constant` | `offset + amplitude` |

With the default `frequency` of 1 every sample falls on a whole cycle: the sine is 0 to
within rounding and the ramp is `offset`. The square wave is then decided by rounding:
`sin(2 pi n)` evaluates to a tiny negative number, so every sample is `offset - amplitude`,
-1 with the defaults (measured with a three-step square wave). Rounding decides the sign
at every other zero crossing too, for example at steps 2 and 4 with `frequency` 0.25. Choose a
frequency that keeps the samples off the zero crossings, such as 0.125, when the square
wave's sign matters.

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Output | `signal` | "Signal", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the sample of the last update; no time stamp |

## Behavior

### initialize()

Latches the five arguments' first values and checks two of them: an unknown `waveform` and
a `steps` below 1 make initialization fail (see Errors). It then creates `signal`, holding 0.
Arguments set after `initialize()` take effect at the next `initialize()`, which also
recreates the output and so drops its connections.

### validate()

Moves through Validating to Valid and returns an empty list.

### prepare()

Resets the step count to 0 and moves to Updated with the message `prepared`. Preparing again
restarts the waveform at step 1.

### update()

Advances the step, writes that step's sample to `signal`, and reports Updated with the
message `stepped`, or Done with the same message once the step count reaches `steps`. It
does not check the state, so a host stops calling it once the component is Done.

### finish()

Moves through Finishing to Finished. `signal` keeps its last sample.

## Status messages

A two-step run:

```text
[signal_source] status messages of a two-step run
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
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

## Errors

Produced by `docs/examples/reference_messages` and read from `errors()` or the argument's
message:

```text
[signal_source] errors
  waveform "triangle"
    Argument initialization failed: waveform "triangle" is not one of sine, square, ramp, constant
  steps 0
    Argument initialization failed: steps must be at least 1, not 0
  amplitude "two"
    argument amplitude: [json.exception.type_error.302] type must be number, but is string
  amplitude with no values
    initialize() threw: vector
  amplitude with no values, through a composition
    JSON parse error: vector
```

| Message | Cause | Fix |
| --- | --- | --- |
| `Argument initialization failed: waveform "<w>" is not one of sine, square, ramp, constant` | `waveform` holds another name, or another case. The status is Failed. | Use one of the four names, in lower case, and initialize again. |
| `Argument initialization failed: steps must be at least 1, not <n>` | `steps` is 0 or negative. | Set `steps` to 1 or more. |
| `[json.exception.type_error.302] type must be number, but is string` | The payload of a numeric argument holds a string; the argument refuses it before the component sees it. A fractional `steps` is truncated toward zero instead of refused. | Write numbers without quotes: `{ "values": [2.0] }`. |
| `initialize()` throws `std::out_of_range` (`vector` with libc++) | An argument's payload has no values, `{ "values": [] }`, and the component reads element 0. Through `ModelInitializer::initializeFromJsonText` the exception is reported as `JSON parse error: vector`. | Give every argument exactly one value. |

The last row is a defect: the component does not check that an argument holds a value
before reading it.

## Example composition

`compositions/source_to_recorder.json` configures a constant source, `offset + amplitude`
= 3 + 2 = 5, for three steps:

\dontinclude source_to_recorder.json
\skip "components"
\until "steps"

The @ref cookbook "cookbook" shows the whole document and its recording. A quarter-cycle
sine, as `SignalChainTest.SignalSourceProducesTheConfiguredWaveform` configures it:

\dontinclude test_signalchain.cpp
\skip SignalSourceProducesTheConfiguredWaveform
\until EXPECT_EQ(source->status(), ComponentStatus::Done);

## Tests

| Test | What it checks |
| --- | --- |
| `SignalChainTest.SignalSourceProducesTheConfiguredWaveform` | a sine of amplitude 2, frequency 0.25 and offset 10 gives 12, 10, 8, 10 and Done exactly at step 4 |
| `SignalChainTest.GainTransformerScalesItsInputEachStep` | a constant source of 3 drives the gain transformer for two steps |
| `SignalChainTest.RecorderWritesOneCsvRowPerStep` | a ramp of amplitude 8 at frequency 0.25 records 2, 4, 6 |
| `CombinerTest.*`, `CompositionsTest.*` | sources as providers for the combiner and for every testing composition |
| `docs.example.reference_messages` | the status messages and errors above |
