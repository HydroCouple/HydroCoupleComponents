# Smoke Component {#ref_smoke}

[TOC]

The smoke component is the smallest complete component in this repository: no arguments,
no inputs, and one output, `produced`, set to 42 in a single step. It exists to prove that
the toolchain, the SDK base classes and the loading convention work before any component
carries an algorithm, and it is the starting point of @ref tutorial_component "the tutorial".
It is a test fixture, not a tool.

| | |
| --- | --- |
| Library | `hcc_smoke`, built as `build/components/libhcc_smoke.so` |
| Component info ID | `hydrocouple.components.smoke` |
| Caption, version, tags | Smoke Component; 0.1.0; Test, Scaffold |
| Default instance ID | `hydrocouple.components.smoke.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent` |
| Source | `components/smoke/smokecomponent.cpp` |

## Arguments

None.

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Output | `produced` | "Answer", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the value of the last update; no time stamp |

## Behavior

### initialize()

The SDK base class creates the (empty) argument set, calls `initializeArguments()`, which
always succeeds, and creates the `produced` output with the value 0. The status ends at
Initialized.

### validate()

Moves through Validating to Valid and returns an empty list.

### prepare()

Moves through Preparing to Updated with the message `prepared`.

### update()

Writes 42 to `produced` and reports Done with the message `produced`. One update is the
whole run. The call does not check the state: a host stops calling it once the component is
Done.

### finish()

Moves through Finishing to Finished. The output keeps its value until the instance is
destroyed.

## Status messages

The status changes of one run, recorded by `docs/examples/reference_messages` through a
status listener:

```text
[smoke] status messages of one run
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Updating ""
    Done "produced"
    Finishing ""
    Finished ""
```

The Initializing and Initialized messages come from the SDK base class; every SDK component
reports them (see @ref concepts "Concepts").

## Errors

The component reports no errors of its own.

## Example composition

`docs/examples/cookbook_compositions/smoke.json` places the component alone:

\include{lineno} smoke.json

The cookbook runner applies it and steps the workflow once; the final output is

```text
smoke.json
  workflow: 1 updates, Done
  smoke.produced = 42
```

## The source

The whole component, from `components/smoke/smokecomponent.cpp`:

\dontinclude smokecomponent.cpp
\skip class SmokeComponent :
\until } // namespace

The file's last line, outside the unnamed namespace, publishes the info:
`HYDROCOUPLE_DECLARE_COMPONENT(SmokeComponentInfo)`.

## Tests

| Test | What it checks |
| --- | --- |
| `SmokePluginTest.TheStampMatchesBeforeAnythingElseIsCalled` | the library's ABI stamp equals the host's, read before any other call |
| `SmokePluginTest.TheInfoCrossesTheImageBoundaryAsAModelComponentInfo` | the info's ID, and the `dynamic_cast` to `IModelComponentInfo` across the `dlopen` boundary |
| `SmokePluginTest.ACreatedInstanceRunsToFinishedAndProduces` | a full run from `initialize()` to Finished, then 42 read from `produced` |
| `docs.example.cookbook_compositions` | `smoke.json` runs to Done with `produced` = 42 |
| `docs.example.reference_messages` | the status messages above |
