# Component reference {#component_reference}

[TOC]

One page per component, each on the same template: what the component is for; its
arguments with type, default, units and valid range; its inputs and outputs with quantity,
geometry and time support; its behavior through `initialize()`, `validate()`, `prepare()`,
`update()` and `finish()`; the status messages of a run; every error message with its cause
and fix; an example composition; and the tests that cover it. The status and error listings
are the output of `docs/examples/reference_messages`, which loads the built plugins, provokes
each message and fails when one changes, so the listings cannot drift from the code.

| Component | Library | Component info ID | Family | Page |
| --- | --- | --- | --- | --- |
| Mesh Generator | `hcc_meshgenerator` | `hydrocouple.components.meshgenerator` | Providers | @ref ref_meshgenerator "Mesh Generator" |
| Time Series Provider | `hcc_timeseriesprovider` | `hydrocouple.components.timeseriesprovider` | Providers | @ref ref_timeseriesprovider "Time Series Provider" |
| Signal Source | `hcc_signalsource` | `hydrocouple.components.signal_source` | Testing | @ref ref_signalsource "Signal Source" |
| Gain Transformer | `hcc_gaintransformer` | `hydrocouple.components.gain` | Testing | @ref ref_gaintransformer "Gain Transformer" |
| Recorder | `hcc_recorder` | `hydrocouple.components.recorder` | Testing | @ref ref_recorder "Recorder" |
| Combiner | `hcc_combiner` | `hydrocouple.components.combiner` | Testing | @ref ref_combiner "Combiner" |
| Basic Adapters | `hcc_basicadapters` | `hydrocouple.components.adapters.basic` | Testing | @ref ref_basicadapters "Basic Adapters" |
| Controllable interval source | `hcc_intervalsource` | `org.hydrocouple.openswmm.test.interval-source` | Interval transfer | @ref interval_transfer "Interval-transfer components" |
| Conservative interval receiver | `hcc_intervalreceiver` | `org.hydrocouple.openswmm.test.sink` | Interval transfer | @ref interval_transfer "Interval-transfer components" |
| Smoke Component | `hcc_smoke` | `hydrocouple.components.smoke` | Scaffold | @ref ref_smoke "Smoke Component" |

Every library is a CMake `MODULE` named `lib<library>.so` on Linux and macOS, written to
`build/components` and installed to `lib/hydrocouple/components`. The pages of the four
families:

- @subpage providers "Provider components": components whose outputs initialize and
  parameterize other components.
- @subpage testing_components "Testing components": deterministic components with known
  answers, for checking hosts and compositions.
- @subpage interval_transfer "Interval-transfer components": the reference pair of the
  OpenSWMM interval-transfer profile.
- @subpage ref_smoke "Smoke Component": the scaffold's proof plugin.

Facts every SDK-based component here shares, such as the SDK's own status messages, how
arguments are given, and where relative paths resolve, are on @ref concepts "Concepts".
