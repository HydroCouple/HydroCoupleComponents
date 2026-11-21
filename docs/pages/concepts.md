# Concepts {#concepts}

[TOC]

This page explains what the plugin libraries are, how a host loads and runs one, and the
two ways a provider's results reach the components it feeds. The pages for each component
family build on it.

## A component library

Each plugin is one shared library holding one component type. The `hcc_add_component`
function in the root `CMakeLists.txt` declares it the same way every time: a CMake
`MODULE` library, linked privately to HydroCoupleSDK, compiled with hidden symbol
visibility, written to `build/components` and installed to `lib/hydrocouple/components`.
The interval-transfer pair is the one exception to "one source file, one library": its
source file is compiled twice, once with `HCC_INTERVAL_SOURCE` defined.

A library publishes its component through the loading convention of the HydroCouple
interface package, `hydrocouplecomponentabi.h`, which the sources include as
`hydrocouplesdk/componentabi.h`. The last line of every source file,
`HYDROCOUPLE_DECLARE_COMPONENT(InfoType)`, emits the only two exported functions:

| Function | Returns |
| --- | --- |
| `hydrocouple_component_abi_v1()` | The ABI stamp: a C string built from the loading convention's revision, the interface ABI version, the compiler family and major version, the standard library and the pointer width. |
| `hydrocouple_component_info_v1()` | The library's component info, an `IComponentInfo` the library owns. |

Hidden visibility is what keeps everything else private: the ABI header marks the two
entry points visible itself, and the interface headers keep their own type information
visible so that a host can cast across the library boundary.

## How a host loads a component

<pre class="mermaid">
sequenceDiagram
    participant Host
    participant Library as Plugin library
    Host->>Library: dlopen
    Host->>Library: hydrocouple_component_abi_v1()
    Library-->>Host: ABI stamp
    Note over Host: refuse the library unless the stamp equals the host's own
    Host->>Library: hydrocouple_component_info_v1()
    Library-->>Host: IComponentInfo, owned by the library
    Note over Host: cast to IModelComponentInfo or IAdaptedOutputFactoryComponentInfo
    Host->>Library: createComponentInstance()
    Library-->>Host: a new instance, owned by the host
    Note over Host: destroy every instance before closing the library
</pre>

The stamp function is plain C and returns a string, so it is safe to call even when the
library was built with a different toolchain. It is the only function a host may call
before the stamps match; a mismatch has no recovery once a C++ function has run. The info
object lives as long as the library is loaded and must not be deleted. Instances belong to
the host and must be destroyed before the library that allocated them is unloaded, which
is why every test in this repository keeps its instances in a narrower scope than its
library handles.

Nine of the ten libraries publish an `IModelComponentInfo`. The Basic Adapters library
publishes an `IAdaptedOutputFactoryComponentInfo` instead, and does not answer as a model
component, so a host lists it as an adapter factory rather than as a component to place
in a composition. `SmokePluginTest` and `BasicAdaptersTest` check both casts across the
library boundary.

## The component lifecycle

The model components derive from the SDK's `AbstractModelComponent` and implement its
hooks. A host drives each one through the same states:

<pre class="mermaid">
stateDiagram-v2
    [*] --> Created
    Created --> Initialized: initialize()
    Created --> Failed: initialize() fails
    Initialized --> Valid: validate()
    Initialized --> Invalid: validate() fails
    Valid --> Updated: prepare()
    Updated --> Updated: update()
    Updated --> Done: last update()
    Updated --> Failed: update() fails
    Done --> Finished: finish()
</pre>

| Call | Base class hooks | What these components do there |
| --- | --- | --- |
| First access to `arguments()` | `createArguments()` | Create every argument with its default value. |
| `initialize()` | `initializeArguments()`, `createInputs()`, `createOutputs()` | Read and check the arguments, then create the ports. On failure the status is Failed, the message is in `errors()` and `initializeFailureCleanUp()` releases partial state. |
| `validate()` | | Return a list of problems; an empty list means Valid. |
| `prepare()` | | Reset step counters and open output files (the recorder's CSV file, the interval receipt). |
| `update()` | | Advance one step. |
| `finish()` | | Close files. |

Three rules follow from how hosts use the lifecycle.

- **Initializing must be cheap.** A host such as Composer initializes a component just to
  learn its ports. The mesh generator therefore triangulates in `update()`, never in
  `initialize()`, and its default arguments mesh a 100 by 100 square so that an
  unconfigured instance is still valid. The time series provider is the deliberate
  exception: its outputs are the columns of its source, so it reads the source while it
  initializes.
- **Arguments come first.** Set arguments, then call `initialize()`. Calling
  `initialize()` again re-reads the arguments but recreates the ports, which discards
  connections made to the old ones, so wire the components after their last
  initialization. The interval-transfer pair goes further and refuses a new `config` once
  initialization has started.
- **Consumers follow their providers' clocks.** The gain transformer, recorder and combiner
  report Done once every connected provider component is Done or Finished, and keep
  reporting Updated before that. The signal source and the interval source decide when the
  run ends.

## How a provider's results reach a consumer

Provider components produce what other components start from: a mesh, a forcing series.
Their results reach a consumer in one of two ways.

<pre class="mermaid">
flowchart LR
    subgraph live [Live, one composition]
        G1[Mesh Generator] -- binding --> A1[mesh argument]
        T1[Time series] -- connection --> I1[input]
    end
    subgraph standalone [Standalone, two runs]
        G2[Mesh Generator] --> R[(Run manifest)]
        R -- mode open --> A2[mesh argument]
    end
</pre>

**Live.** An argument in a composition document can name another component's output
instead of a value:

```json
"mesh": { "@from": { "component": "meshgen", "output": "mesh" } }
```

The SDK's `ModelInitializer` applies the components of a document in an order that puts
every bound provider before its consumer. For a binding it runs the provider to Finished (validate, prepare, update until Done,
finish) and hands the named output to the consumer's argument before the consumer
initializes. `MeshGeneratorTest` binds a mesh this way, `TimeSeriesProviderTest` binds a
series, and `compositions/bound_arguments.json` binds the gain transformer's `bias` to a
signal. Outputs can also feed inputs through the document's `connections`, optionally
through adapted outputs, and a workflow then steps the components together.

**Standalone.** A provider runs on its own and its results are kept. The mesh generator
can write its mesh to a UGRID file (`output_path`). A host can also record a run in a run
manifest; a later composition then marks that component
`"execution": { "mode": "open", "results": "<manifest>" }` and `ModelInitializer` serves
the recorded outputs in its place. `compositions/demo_b_meshgen.json` and
`compositions/demo_b_open.json` are that pair for the mesh generator; the ARGGEN demo
suite that runs them needs FVQual's plugin and was not run for this manual.

## Component families

| Family | Components | Page |
| --- | --- | --- |
| Providers | Mesh Generator, Time Series Provider | @ref providers "Provider components" |
| Testing | Signal Source, Gain Transformer, Recorder, Combiner, Basic Adapters | @ref testing_components "Testing components" |
| Interval transfer | Controllable interval source, conservative interval receiver | @ref interval_transfer "Interval-transfer components" |
| Scaffold | Smoke Component | below |

The README's plans also name a terrain sampler. It is not in this repository yet.

## Namespaces and the reference

Each source file keeps all of its classes and helpers in an unnamed namespace, so they
have internal linkage: two plugins can define classes with the same name, and nothing but
the two entry points leaves a library. The reference lists each of these namespaces as
`anonymous_namespace{<file>}`, one per plugin source file, under
[Namespaces](namespaces.html); [Classes](annotated.html) lists the classes inside them.

The sources also use names from two namespaces this repository does not define. Every
file opens both with `using namespace`:

- `HydroCouple`: the interface standard (`IModelComponent`, `IArgument`, `IInput`,
  `IOutput`, `IAdaptedOutputFactory` and the rest), documented in the
  [interface standard manual](https://www.hydrocouple.org/HydroCouple/).
- `HydroCouple::SDK`: the base classes the components build on (`AbstractModelComponent`,
  `AbstractModelComponentInfo`, `Argument1DDouble`, `Input1DDouble`, `Output1DDouble` and
  the rest), documented in the [SDK manual](https://www.hydrocouple.org/HydroCoupleSDK/).

## Anatomy of a component: the smoke plugin

`hcc_smoke` is the scaffold's proof that the toolchain and the loading convention work
before any component carries an algorithm. It has one output, `produced`, set to 42 in a
single step. It is a test fixture, not a tool. Its whole component, from
`components/smoke/smokecomponent.cpp`:

\dontinclude smokecomponent.cpp
\skip class SmokeComponent :
\until HYDROCOUPLE_DECLARE_COMPONENT

The component class implements the lifecycle calls and the four creation hooks; the info
class carries the metadata and creates instances; the macro publishes the info.
`SmokePluginTest` loads the library with `dlopen`, checks the stamp, casts the info, runs
an instance from `initialize()` to Finished and reads 42 from its output.
