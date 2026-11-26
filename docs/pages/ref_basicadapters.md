# Basic Adapters {#ref_basicadapters}

[TOC]

Basic Adapters is an adapted-output factory, not a model component. An adapted output sits
between an output and the input it feeds and transforms the values on the way; a factory
offers adapted outputs for a given output and creates them. This library offers two,
`negate` and `clamp`, for any rank-1 Float64 output. They are deliberately different from
the SDK's own factory (`hydrocouple.sdk.adapters`), so a composition that names them proves
that the host consulted this library.

| | |
| --- | --- |
| Library | `hcc_basicadapters`, built as `build/components/libhcc_basicadapters.so` |
| Component info ID | `hydrocouple.components.adapters.basic` |
| Caption, version, tags | Basic Adapters; 1.0.0; Test, Adapters |
| Factory instance ID | `hydrocouple.components.adapters.basic`, the info ID itself |
| Interfaces | info: `IAdaptedOutputFactoryComponentInfo`; instance: `IAdaptedOutputFactoryComponent`, an `IAdaptedOutputFactory`; created items: `IAdaptedOutput` |
| Source | `components/basicadapters/basicadapterscomponent.cpp` |

The info does not answer as an `IModelComponentInfo`, so a host lists the library among
its adapter factories and never places it in a composition as a component. Composer
refuses to create it as one, with
``'hydrocouple.components.adapters.basic' is an adapted-output factory; adapters attach to connections, they are not placed as components``.
The factory instance carries the info's ID, not `<id>.instance`, because a
composition's `adapted_outputs` entries name the factory by the ID of the realized
factory.

## Offerings and their arguments

| Adapted output | Argument | Type | Default | Units | Valid range | Result, element by element |
| --- | --- | --- | --- | --- | --- | --- |
| `negate` | none | | | | | `-x` |
| `clamp` | `minimum` | Float64 | -1e30 | the adaptee's | at most `maximum` | `x` limited to [`minimum`, `maximum`] |
| | `maximum` | Float64 | 1e30 | the adaptee's | at least `minimum` | |

The `clamp` bounds are not checked against each other. With `minimum` above `maximum`,
`std::clamp`'s result is undefined; keep `minimum <= maximum`.

## Inputs and outputs

A created adapted output is itself an output:

| Property | Value |
| --- | --- |
| ID | `<offering>#<n>`, n counting this factory instance's creations from 1, for example `negate#1` |
| Data kind and shape | Float64, the adaptee's shape |
| Quantity | none (null value definition) |
| Time support | the adaptee's values at the last refresh |
| Writable | no |

Both offerings accept any rank-1 Float64 output, adapted outputs included, so they chain:
the clamp can adapt the negate's output. The consumer offered to the factory is not
considered.

## Behavior

### Offering

`getAvailableAdaptedOutputIds(provider)` returns `negate` and `clamp` for a rank-1
Float64 provider of any length, and nothing for any other provider (a String output, the
mesh generator's geometry-only `mesh`).

### Creation

`createAdaptedOutput(offering, provider)` returns a new adapted output, owned by the
caller, already registered with its provider as the interface requires; it returns null
for a null argument or an unknown offering. The registration does not own the adapted
output: whoever owns it calls `provider->removeAdaptedOutput()` before destroying it, inner
links of a chain first. `ModelInitializer` owns the adapted outputs a composition creates
and keeps them for its own lifetime.

### initialize()

Reads the arguments (for `clamp`, the first value of `minimum` and `maximum`), then
refreshes. Set arguments before `initialize()`; `ModelInitializer` applies a composition's
adapter arguments in that order.

### refresh()

Reads the whole adaptee, transforms each value, signals the change and refreshes every
adapted output chained to this one. When the adaptee is missing, is not rank 1, or refuses
the read, the previous values stay and nothing is signaled. `updateValues()` does the same.
A time-stepped workflow refreshes the first adapted output of each chain after its
provider's component updates; the chain refreshes the rest.

## Errors

```text
[basic_adapters] errors
  reading before initialize()
    adapted output "negate#1" has no values; initialize() has not run
  setValuesFrom
    adapted outputs are read-only
  negate requested for the mesh output
    connections[0]: no adapted output "negate" is offered for output "mesh" (searched 0 factories on component "hydrocouple.components.meshgenerator.instance" and 1 standalone factories, factory "hydrocouple.components.adapters.basic" requested)
```

| Message | Cause | Fix |
| --- | --- | --- |
| `adapted output "<id>" has no values; initialize() has not run` | The adapted output was read before `initialize()`, or its adaptee has length 0. | Call `initialize()` after setting its arguments. |
| `adapted outputs are read-only` | Code tried to write an adapted output. | Write the adaptee instead. |
| `connections[<i>]: no adapted output "<id>" is offered for output "<o>" (searched ...)` | `ModelInitializer` found no factory offering that ID for that output: the output is not rank-1 Float64, the ID is misspelled, or the factory is not installed. | Check the output's kind and shape, the `id` and `factory` fields, and that the host installed this factory (see below). |
| ``connections[<i>]: adapted output "<id>" requested but the provider's component exposes no adapter factories and no standalone adapter factories are installed`` | The host gave `ModelInitializer` no factories at all. | Install the factory with `setAdapterFactoryResolver()`. |
| `connections[<i>]: adapted output "clamp" has no argument "<a>"` | The composition names an argument the offering does not have. | Use `minimum` and `maximum`; `negate` has none. |

## Installing the factory in a host

`ModelInitializer` reaches standalone factories through its adapter factory resolver. The
host loads the library, creates the factory instance and returns it from the resolver,
beside the SDK's own. This is how `CompositionsTest` does it:

\dontinclude test_compositions.cpp
\skip initializer.setAdapterFactoryResolver(
\until });

The factory instance must outlive every adapted output it made, and both must be destroyed
before the library is unloaded.

## Example composition

`compositions/source_gain_recorder_adapted.json` passes a ramp through `negate` and then
`clamp` before the gain transformer:

\dontinclude source_gain_recorder_adapted.json
\skip "connections"
\until "to": { "component": "gain", "input": "in" } },

The ramp 2, 4, 6 becomes −2, −4, −6, then −3, −4, −5 inside [−5, −3], and the gain
transformer's 2x + 1 records −5, −7, −9. The @ref cookbook "cookbook" shows the run.

Driven directly, the factory, a negate and a clamp chained to it, as
`BasicAdaptersTest.NegateAndClampAdaptChainableOneDimensionalOutputs` does:

\dontinclude test_basicadapters.cpp
\skip Output1DDouble produced(
\until EXPECT_TRUE(produced.removeAdaptedOutput(negate.get()));

## Tests

| Test | What it checks |
| --- | --- |
| `BasicAdaptersTest.AdapterFactoryPluginLoadsThroughTheStampedAbi` | the stamp; the info answers as a factory info and not as a model component info; the instance's ID is the info ID |
| `BasicAdaptersTest.NegateAndClampAdaptChainableOneDimensionalOutputs` | the offerings; negate of (3, −8, 5); clamp of the negate to [−4, 4]; a refresh reaching through the chain; unregistering inner links first |
| `CompositionsTest.AdaptedCompositionNegatesAndClampsThroughTheChain` | the chain in a composition records −5, −7, −9 |
| `docs.example.reference_messages` | the errors above |
