# Concepts {#concepts}

[TOC]

This page explains what the plugin libraries are, how a host loads and runs one, how
arguments and exchange items behave, how a composition becomes a run, and the two ways a
provider's results reach the components it feeds. The component pages build on it. The
interfaces themselves are documented in the
[HydroCouple interface manual](https://www.hydrocouple.org/HydroCouple/) and the base
classes in the [HydroCouple SDK guide](https://www.hydrocouple.org/HydroCoupleSDK/).

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
| `hydrocouple_component_abi_v1()` | The ABI stamp: a C string built from the loading convention's revision, the interface ABI version, the compiler family and major version, the standard library and its ABI settings, on Windows the C runtime, and the pointer size. |
| `hydrocouple_component_info_v1()` | The library's component info, an `IComponentInfo` the library owns: a function-local static of `InfoType`, created at the first call. |

Hidden visibility is what keeps everything else private: the ABI header marks the two
entry points visible itself, and the interface headers keep their own type information
visible so that a host can cast across the library boundary. Inside each source file every
class sits in an unnamed namespace (see Namespaces below).

A plugin built for this manual stamps itself

```text
hc-abi/2;iface=5;cxx=clang-21;stdlib=libc++1;ptr=8
```

@ref troubleshooting "Troubleshooting" explains each field and what changes it.

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

The plugins link HydroCoupleSDK as a shared library and expect the host to have loaded the
same build: an installed plugin carries no runtime search path of its own. The
@ref tutorial_component "tutorial" writes a host's loading code, and
@ref troubleshooting "Troubleshooting" lists what fails when the libraries do not match.

Nine of the ten libraries publish an `IModelComponentInfo`. The Basic Adapters library
publishes an `IAdaptedOutputFactoryComponentInfo` instead, and does not answer as a model
component, so a host lists it as an adapter factory rather than as a component to place
in a composition. `SmokePluginTest` and `BasicAdaptersTest` check both casts across the
library boundary.

## Instances, IDs and the reference directory

A component info creates instances with a default ID: `<info ID>.instance` for the SDK-based
components here, `interval-source` and `interval-receiver` for the interval pair. A host
that places several instances, or records results, gives each the ID of its block in the
composition before initialization; Composer does it with the SDK's
`AbstractModelComponent::setInstanceId()`, which refuses an empty ID and any call after the
Created state. Messages that name a component by its instance ID, such as
`component "hydrocouple.components.gain.instance" has no input "x"`, show the default ID when
the host did not assign one.

Each instance also has a reference directory, set with `setReferenceDirectory()`, against
which some components resolve relative file paths. Composer sets it to the composition
document's directory, the tests and examples to their output directories. Not every
component uses it:

| Component | Argument | A relative path is relative to |
| --- | --- | --- |
| Recorder | `path` | the reference directory when set, else the working directory |
| Interval receiver | `receipt_file` | the reference directory (the working directory when it is empty) |
| Time Series Provider | `source`, `cache` | the process's working directory |
| Mesh Generator | `output_path` | the process's working directory |

## The component lifecycle

The model components derive from the SDK's `AbstractModelComponent` and implement its
hooks. A host drives each one through the same states:

<pre class="mermaid">
stateDiagram-v2
    [*] --> Created
    Created --> Initializing: initialize()
    Initializing --> Initialized
    Initializing --> Failed
    Initialized --> Validating: validate()
    Validating --> Valid
    Validating --> Invalid
    Valid --> Preparing: prepare()
    Preparing --> Updated
    Updated --> Updating: update()
    Updating --> Updated
    Updating --> Done
    Updating --> Failed
    Done --> Finishing: finish()
    Updated --> Finishing: finish()
    Finishing --> Finished
</pre>

The diagram shows the path these components take. The interfaces define the complete table
(`HydroCouple::Helpers::isValidComponentStatusTransition`): Failed is reachable from any
state but Finished; Initialized, Valid and Invalid may also go to Finishing; Valid and
Invalid may be validated again; Initialized and Failed may be initialized again. The SDK
performs any transition a component asks for, but queues `illegal status transition <from>
-> <to> (Helpers::isValidComponentStatusTransition rejects it)` on `errors()` the first time
a component makes one the table rejects. Two such reports come from these plugins and are
listed in @ref troubleshooting "Troubleshooting".

| Call | Base class hooks | What these components do there |
| --- | --- | --- |
| First access to `arguments()` | `createArguments()` | Create every argument with its default value. |
| `initialize()` | `initializeArguments()`, `createInputs()`, `createOutputs()` | Read and check the arguments, then create the ports. On failure the status is Failed, the message is in `errors()` and `initializeFailureCleanUp()` releases partial state. |
| `validate()` | | Return a list of problems; an empty list means Valid. |
| `prepare()` | | Reset step counters and open output files (the recorder's CSV file, the interval receipt). |
| `update()` | | Advance one step. |
| `finish()` | | Close files. |

`addArgument()`, `addInput()` and `addOutput()` only register an item with the base class:
`AbstractModelComponent` lists it, and its destructor and `initialize()`, which empties the
input and output lists before recreating the ports, never delete it. The component must own
its items. The interval pair does, in members of its own; the other plugins create their
items with `new` and only register them, so an instance's arguments and ports are not freed
when it is destroyed, and each further `initialize()` leaves the previous ports behind.
The @ref tutorial_component "tutorial" shows the owning pattern. The SDK's
`Quantity::unitLess()` also allocates a `Unit` the quantity does not own, which nothing
frees.

`AbstractModelComponent::initialize()` runs only from Created, Initialized or Failed, and
reports two status messages of its own around the component's hooks:

| Status | Message |
| --- | --- |
| Initializing | `Initializing component` |
| Initialized | `Component initialized successfully` |
| Failed | `Argument initialization failed: ` followed by the component's message |

The component pages list the messages each component adds. A status message is carried by
the status-change event a host listens to (`IModelComponent` is an `ISignal` of
`IComponentStatusChangeEventArgs`); a component's errors are queued on `errors()`, each an
`ErrorEntry` with a severity, a code, the component's ID and the message. Moving to Failed
with a message queues that message with the severity Fatal, so a host that reads `errors()`
after a failure always finds the reason.

Three rules follow from how hosts use the lifecycle.

- **Initializing must be cheap.** A host such as Composer initializes a component only to
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

None of the components is safe to drive from two threads at once; a host calls each
instance from one thread at a time, as the SDK's workflows do.

## Arguments

An argument is a typed, shaped data item a host sets before `initialize()`. Most arguments
here are SDK `Argument1D` items (`Argument1DInt`, `Argument1DDouble`, `Argument1DString`) of
length 1 on a dimension named `scalar`; the mesh generator's `boundary` is a 2-D table. A
composition sets an argument with a JSON payload, which the argument's
`initialize(text, JSON, message)` accepts in these forms:

| Payload | Meaning |
| --- | --- |
| `{ "values": [2.0] }` | the values, as an array; the usual form |
| `[2.0]` | the same, without the object |
| `{ "@uri": "values.json" }` | read the values from a file or URL; a relative path is resolved against the composition's directory when the document was read from a file (`initializeFromFile()`), and against the working directory otherwise |
| `{ "@from": { "component": "c", "output": "o" } }` | a binding: the value is component `c`'s output `o` after `c` ran (see below) |

The array replaces the argument's values and sets its length, whatever the length was:
`{ "values": [1, 2] }` makes a scalar argument two values long, and `{ "values": [] }` makes
it empty. The components read element 0, so extra values are ignored, and an empty argument
makes the read throw (see the @ref ref_signalsource "signal source" page). A value of the
wrong JSON type is refused by the argument, before the component sees it, with the JSON
library's message, for example `[json.exception.type_error.302] type must be number, but is
string`. A String argument's Quality, such as the signal source's waveform names, only
lists choices for a host; the component checks the value in `initialize()`.

The interval pair's `config` is the exception: it takes the configuration object itself,
recognized by its `schema` key.

## Exchange items

Inputs and outputs are exchange items. The testing family's are SDK `Input1DDouble` and
`Output1DDouble` items: rank 1, Float64, one value. An `Input1D` accepts a provider when the
provider's data kind equals its own and the provider is rank 1; it does not compare units,
quantities or lengths. At `applyInputValues()` it copies as many of the provider's leading
values as it holds, so a length-1 input reads the provider's first value. When the read
fails, the input keeps its previous values.

Other items here: the combiner's multi-input `operands`, which takes providers by role; the
mesh generator's geometry-only polyhedral-surface output; the time series provider's
time-series outputs, one value per time step; and the interval pair's typed fields, which
also compare units and dimensions. Adapted outputs sit between an output and the inputs it
feeds and transform the values; the Basic Adapters library offers two.

## From a composition to a run

A composition document names component instances, their arguments, the connections between
their ports, and the workflow. The SDK's `IO::ModelInitializer` applies a document to
instances the host has created, found through a resolver the host supplies:

1. It parses the document and validates it against the live instances: every component ID
   resolves and every argument exists; for components already initialized, every role a
   connection names is declared.
2. It applies the components in stages: in document order, except that a component an
   `@from` binding names comes before the component that binds it. For each component it
   sets the arguments, and for a binding first runs the provider to Finished (validate,
   prepare, update until Done, finish) and initializes the argument from the provider's
   output. Then it calls the component's `initialize()`, which must reach Initialized.
3. It wires the connections, building each connection's chain of adapted outputs from the
   component's own and the host's standalone adapter factories, and connects the last link
   to the input with `addConsumer()`, which calls the input's `canConsume()` and refuses an
   incompatible provider.

The host then adds the instances to a workflow. The SDK's `TimeSteppedWorkflow` initializes
any component still Created or Failed, validates and prepares every component, and orders
them so that providers update before their consumers. Each workflow update advances every
component that is not yet Done or Finished once, and refreshes the adapted outputs of the
outputs it updated. The workflow is Done when every component is Done or Finished, and
it stops with ``component '<id>' failed during update`` at the first component that fails. The
host then calls `finish()` on each component. The @ref cookbook "cookbook" shows each step's
effect on real documents.

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

`ModelInitializer` runs the provider to Finished and hands the named output to the
consumer's argument before the consumer initializes. `MeshGeneratorTest` binds a mesh this
way, `TimeSeriesProviderTest` binds a series, and `compositions/bound_arguments.json` binds
the gain transformer's `bias` to a signal. Outputs can also feed inputs through the
document's `connections`, optionally through adapted outputs, and a workflow then steps the
components together.

**Standalone.** A provider runs on its own and its results are kept. The mesh generator
can write its mesh to a UGRID file (`output_path`). A host can also record a run in a run
manifest; a later composition then marks that component
`"execution": { "mode": "open", "results": "<manifest>" }` and `ModelInitializer` serves
the recorded outputs in its place, through an SDK `ResultsModelComponent`.
`compositions/demo_b_meshgen.json` and `compositions/demo_b_open.json` are that pair for the
mesh generator; `DemoTest` runs them with the FVQual solver and checks that the results
equal those of the live `demo_a.json`.

## Component families

| Family | Components | Page |
| --- | --- | --- |
| Providers | Mesh Generator, Time Series Provider | @ref providers "Provider components" |
| Testing | Signal Source, Gain Transformer, Recorder, Combiner, Basic Adapters | @ref testing_components "Testing components" |
| Interval transfer | Controllable interval source, conservative interval receiver | @ref interval_transfer "Interval-transfer components" |
| Scaffold | Smoke Component | @ref ref_smoke "Smoke Component" |

The README's plans also name a terrain sampler. It is not in this repository yet.

## Namespaces and the reference

Each source file keeps all of its classes and helpers in an unnamed namespace, so they
have internal linkage: two plugins can define classes with the same name, and nothing but
the two entry points leaves a library. The reference lists each of these namespaces as
`anonymous_namespace{<file>}`, one per plugin source file, under
[Namespaces](namespaces.html); [Classes](annotated.html) lists the classes inside them. The
descriptions of those namespaces come from `docs/namespace_docs_filter.py`, which adds one
to each source file as Doxygen reads it.

The sources also use names from two namespaces this repository does not define. Every
file opens both with `using namespace`:

- `HydroCouple`: the interface standard (`IModelComponent`, `IArgument`, `IInput`,
  `IOutput`, `IAdaptedOutputFactory` and the rest), documented in the
  [interface standard manual](https://www.hydrocouple.org/HydroCouple/).
- `HydroCouple::SDK`: the base classes the components build on (`AbstractModelComponent`,
  `AbstractModelComponentInfo`, `Argument1DDouble`, `Input1DDouble`, `Output1DDouble` and
  the rest), documented in the [SDK manual](https://www.hydrocouple.org/HydroCoupleSDK/).
