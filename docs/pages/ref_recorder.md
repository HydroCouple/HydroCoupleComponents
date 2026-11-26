# Recorder {#ref_recorder}

[TOC]

The recorder writes what arrives at its input to a CSV file, one `step,value` row per
update, so a run leaves a file a person can open. It is the testing family's sink: every
composition in `compositions/` that the test suite checks ends in a recorder, and the
checks compare its file row by row.

| | |
| --- | --- |
| Library | `hcc_recorder`, built as `build/components/libhcc_recorder.so` |
| Component info ID | `hydrocouple.components.recorder` |
| Caption, version, tags | Recorder; 1.0.0; Test, Sink |
| Default instance ID | `hydrocouple.components.recorder.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent` |
| Source | `components/recorder/recordercomponent.cpp` |

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `path` | String | empty | none | required: not empty | The CSV file to write. A relative path is joined to the component's reference directory when one is set, and otherwise stays relative to the process's working directory. The argument carries the file filter `CSV (*.csv)` for hosts that offer a file dialog. |

The reference directory is set by the host before the arguments are applied: Composer sets
it to the composition document's directory, `CompositionsTest` sets it to
`build/test-artifacts`, and the cookbook runner to its working directory.

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Input | `in` | "Recorded", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the provider's value at this update |

`in` accepts any rank-1 Float64 provider and records only its first value. The component
has no outputs.

## Behavior

### initialize()

Requires a nonempty `path` and resolves it (see the table above). The file is not opened
yet, so initializing a recorder, as a host does to learn its ports, creates nothing.

### validate()

Moves to Valid.

### prepare()

1. Resets the step count to 0.
2. Creates the file, or truncates an existing one. It does not create missing directories.
3. Writes the header `step,value` and moves to Updated with the message `prepared`.

If the file cannot be opened, the component queues the error `cannot open "<path>" for
writing` (code 1) and moves to Failed with the message `the recording file would not
open`; the SDK queues that message too, with the severity Fatal.

### update()

1. Pulls the provider's first value into `in`.
2. Appends the row `<step>,<value>`, counting steps from 1. Values use the C++ stream's
   default format, six significant digits: 1.23456789 is written as `1.23457`, and
   2440587.5 as `2.44059e+06`.
3. Reports Done with the message `recorded` when the provider's component is Done or
   Finished, or when `in` has no provider; otherwise Updated.

Rows are buffered by the stream and reach the disk when the buffer fills or the file
closes.

### finish()

Closes the file and moves to Finished. A recorder that is never finished leaves its last
rows in the stream's buffer until the instance is destroyed.

## Status messages

A one-step run with nothing connected:

```text
[recorder] status messages of a one-step run
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Updating ""
    Done "recorded"
    Finishing ""
    Finished ""
```

## Errors

```text
[recorder] errors
  path empty (the default)
    Argument initialization failed: path is required: the recorder exists to leave a file behind
  path below a regular file
    cannot open "recorder_run.csv/recorded.csv" for writing
    the recording file would not open
```

| Message | Cause | Fix |
| --- | --- | --- |
| `Argument initialization failed: path is required: the recorder exists to leave a file behind` | `path` is empty, its default. The status is Failed after `initialize()`. | Set `path`, then initialize again. |
| `cannot open "<path>" for writing`, then `the recording file would not open` | `prepare()` could not create the file: a directory in the path does not exist or is a file, or the directory is not writable. The status is Failed. | Create the directory, or choose another path. Check the printed path: it is the resolved one, so a relative path shows where the reference directory put it. |

## Example composition

`compositions/source_to_recorder.json` records a constant source for three steps:

\include{lineno} source_to_recorder.json

The run leaves `recorded_direct.csv` in the reference directory:

```text
step,value
1,5
2,5
3,5
```

The @ref cookbook "cookbook" lists the other recordings.

## Tests

| Test | What it checks |
| --- | --- |
| `SignalChainTest.RecorderWritesOneCsvRowPerStep` | an empty path fails initialization; with a path, a three-step ramp records `1,2`, `2,4`, `3,6` and nothing more |
| `CompositionsTest.*` | the four testing compositions' recordings, row for row |
| `docs.example.cookbook_compositions` | the same recordings, through a host-style runner |
| `docs.example.reference_messages` | the status messages and errors above |
