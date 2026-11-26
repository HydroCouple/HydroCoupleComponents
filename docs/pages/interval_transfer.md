# Interval-transfer components {#interval_transfer}

[TOC]

The interval-transfer pair is a reference implementation of the OpenSWMM interval-transfer
profile, `hydrocouple.interval-transfer/1`: a convention for handing conserved amounts of
water, heat and dissolved species from one component to another, one time interval at a
time, with a receipt for every interval accepted. The profile adds conventions on top of
the HydroCouple interfaces without changing them. The pair is a test reference for
developers of such components; it is not a hydraulic solver and it does not replace an
OpenSWMM engine wrapper.

| | Source | Receiver |
| --- | --- | --- |
| Library | `hcc_intervalsource` | `hcc_intervalreceiver` |
| Component info ID | `org.hydrocouple.openswmm.test.interval-source` | `org.hydrocouple.openswmm.test.sink` |
| Caption | Controllable interval source | Conservative interval receiver |
| Default instance ID | `interval-source` | `interval-receiver` |
| Version, tags, description | not set | not set |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent` | the same |

Both libraries are built from `components/intervalreceiver/intervalcomponents.cpp`; the
source's build defines `HCC_INTERVAL_SOURCE`. The page
[Developing an interval-transfer component](../INTERVAL_TRANSFER_COMPONENTS.md) is the
developer guide to the profile, including what a custom component must test before it
claims to implement it. This page is the reference for the two components as committed.
Unlike the other plugins, the pair owns its arguments and ports in members of its own and
frees them, and the ports of an earlier initialization, itself; it also owns the units of
its quantities.

## How one interval moves

<pre class="mermaid">
sequenceDiagram
    participant Host
    participant Source as Interval source
    participant Receiver as Interval receiver
    Host->>Source: update()
    Note over Source: publish transfer_id and the interval's fields
    Host->>Receiver: update()
    Receiver->>Source: read every field, then transfer_id again
    Note over Receiver: check the tuple, accept all of it or a fraction
    Note over Receiver: append a receipt row, then publish the accepted amounts
</pre>

The host advances the source once and then the receiver once for each interval; a
time-stepped workflow does this because the receiver's inputs make the source its provider.
Reading the source's outputs never advances it. The source is an offer generator: it does
not model a donor or debit any inventory, and what the receiver does not accept stays the
donor's responsibility.

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `config` | String holding a JSON object; required | empty | none | the configuration below | Describes the species, and for the source the intervals, for the receiver the run's expectations. Apply it before `initialize()`; once the status has left Created it is refused, so configure a new instance to change the ports. Serializing it as JSON returns the same object. |
| `scale` | Float64, optional, role Parameter | 1 | none | finite, at least 0; the source accepts only 1 | On the receiver it multiplies the cumulative outputs only; the per-transfer accepted amounts stay physical. Conservation requires 1, and other values only exercise argument handling. |

In a composition, `config` takes the JSON object itself, not a `values` array: the
argument recognizes an object carrying `schema` and stores it whole. `scale` takes the
usual `{ "values": [1] }`. Both must hold exactly one value.

## Configuration

Both components read:

| Key | Value |
| --- | --- |
| `schema` | `"hydrocouple.interval-transfer/1"` |
| `epoch_julian_day` | Finite Julian day that interval times count from. |
| `species` | Array of `{ "id": ..., "basis": ... }`. Each `id` is nonempty, unique, and made of letters, digits, `_`, `-` and `.`; each `basis` is `kg` or `kg-N`. The array may be empty. |

The source also reads `intervals`, an array of objects with `start_s`, `end_s`,
`water_m3`, `enthalpy_j` and `species`, an object holding an amount for each configured
species and no others. The intervals must start at 0 and be contiguous, each ending after
it starts. All values must be finite, water and species amounts nonnegative (enthalpy is
relative and may be negative), and an interval with no water must carry no heat and no
species. At least one interval is required.

The receiver also reads:

| Key | Value |
| --- | --- |
| `expected_transfers` | Positive integer: the number of intervals the run must deliver. |
| `receipt_file` | Nonempty path of the receipt CSV file. A relative path is joined to the component's reference directory. |
| `max_water_m3` | Optional finite, nonnegative capacity per offered interval. Without it there is no limit. |

A missing required key, or a value of the wrong JSON type, fails with the JSON library's
message, for example ``[json.exception.out_of_range.403] key 'epoch_julian_day' not found``.

## Inputs and outputs

The source's outputs and the receiver's inputs are the same fields, created from the
configuration. Every item holds one host-memory value, shape `[1]` on the dimension
`scalar`, and carries the description `hydrocouple.interval-transfer/1;field=<field ID>`.
Each numeric field has a quantity with its unit, its SI dimensions and its value kind. No
field has a geometry; the time support of every field is the interval it describes.

| Field ID | Type | Unit | Dimensions | Value kind |
| --- | --- | --- | --- | --- |
| `transfer_id` | String | | | |
| `epoch_julian_day` | Float64 | `day` (86400 s) | time | intensive |
| `interval_start_s`, `interval_end_s` | Float64 | `s` | time | intensive |
| `water_m3` | Float64 | `m3` | length cubed | extensive |
| `enthalpy_j` | Float64 | `J` | mass, length squared, time to the power -2 | extensive |
| `species.<id>` | Float64 | `kg` or `kg-N`, as declared | mass | extensive |

The source publishes `transfer_id` as `<instance ID>:<n>`, counting intervals from 1.

The receiver adds these outputs, which keep the units of the fields they report:

| Output ID | Contents |
| --- | --- |
| `accepted_transfer_id` | ID of the last accepted transfer. |
| `accepted.water_m3`, `accepted.enthalpy_j`, `accepted.species.<id>` | Amounts accepted from the last transfer. |
| `cumulative.water_m3`, `cumulative.enthalpy_j`, `cumulative.species.<id>` | Running totals of the accepted amounts, multiplied by `scale`. |
| `rejected_water_m3` | Water offered in the last transfer and not accepted. |

### Connecting the pair

Connect each receiver input to the source output with the same ID. The order in which the
two configurations list their species does not matter. A receiver input accepts a provider
only when it is a direct output, not an adapted one, with the same ID, description, data
type and shape `[1]`, and, for numeric fields, the same value kind, unit label, conversion
to SI and dimensions. A field with another basis, for example `kg` offered to a `kg-N`
input, is refused with the message below.

## Behavior

### initialize()

Parses and checks the configuration, builds the field list and the quantities, and creates
the ports it describes. It does not advance anything or create files: a host can
initialize a receiver to discover its species without a receipt appearing. On failure the
status is Failed and no ports exist.

### validate()

The source moves to Valid. The receiver checks its connections and moves to Valid, or to
Invalid with the problem returned and queued on `errors()`. The connection checks, also
repeated at `prepare()` and at every update, are that:

- every input is connected, and all of them to the same donor component, which is not the
  receiver itself and is neither Failed nor Finished;
- every `species.` output of the donor has a matching receiver input, so no species is
  dropped;
- each donor output feeds this receiver input alone and has no adapted outputs.

The profile therefore allows one donor and one physical receiver, with no temporal or
spatial adapters between them.

### prepare()

The source resets its interval index and moves to Updated. The receiver checks its
connections again, creates the receipt file and any missing parent directories, truncates
it, writes the header, and moves to Updated; it moves to Failed when the file cannot be
opened or the header cannot be written.

### update()

The source, when Updated, publishes the next interval and its `transfer_id`, and reports
Done after its last interval and Updated before. Called in any other state it only queues
the error `Source is not ready to update`.

The receiver:

<pre class="mermaid">
flowchart TD
    A[update] --> B{Updated?}
    B -- no --> B1[error queued]
    B -- yes --> C[read the tuple]
    C --> D{ID seen before?}
    D -- same as last --> D1[acknowledge again]
    D -- changed or stale --> F[Failed]
    D -- new --> E{checks pass?}
    E -- no --> F
    E -- yes --> G[accept a fraction]
    G --> H[write the receipt]
    H --> I[publish amounts]
    I --> J{last transfer?}
    J -- yes --> K[Done]
    J -- no --> L[Updated]
</pre>

The checks, in order: the status is Updated, or the call only queues an error; the
connections are as described above; `transfer_id` did not change while the fields were
read and is nonempty, without line breaks; every value is finite, with water and species
amounts nonnegative; the epoch matches the configuration; the interval starts where the
last accepted one ended and ends after it starts; an interval with no water carries no
heat and no species; and the donor reports Done exactly when this is the last expected
transfer.

- **All or a fraction, never part.** When the offered water exceeds `max_water_m3`, water,
  heat and every species are accepted at the same fraction, `max_water_m3 / water_m3`.
  With a capacity of 0 nothing is credited and all the water is reported as rejected.
- **Nothing is credited before every check passes and the receipt is written.** A failed
  check sets the status to Failed with the reason and leaves every output as it was. A
  receipt that cannot be written stops the run; this profile has no retry or restart.
- **Replays are recognized.** An identical repeat of the most recent transfer keeps its
  acknowledgment without a second credit or receipt row. A changed repeat, a stale ID, or
  an interval that does not start where the last accepted one ended fails.
- **Completion must agree.** The donor must report Done exactly on the receiver's
  `expected_transfers`-th transfer; a mismatch is diagnosed before credit.
- **One direction only.** Negative water would be a reversal, which this receiver does not
  support, and is rejected.

The components advertise no capabilities, checkpointing and cloning included.

### finish()

Both close the receipt stream (the source has none) and move to Finished; a second
`finish()` does nothing.

### The receipt file

The header's columns are

`instance_id`, `transfer_id`, `epoch_julian_day`, `start_s`, `end_s`, `offered_water_m3`,
`accepted_water_m3`, `rejected_water_m3`, `scale`, `accepted_enthalpy_j`

followed by one `species.<id>_<basis>` column of accepted amounts per species, in the
receiver's configured order. Each accepted transfer adds one row and is flushed at once.
IDs are quoted, and numbers are written with up to 17 significant digits. This is the
receipt from `IntervalReceiverTest.PartialAcceptanceScalesTheWholeTupleAndRetainsRejection`,
where a capacity of 1 m3 accepts half of a 2 m3 interval:

```text
instance_id,transfer_id,epoch_julian_day,start_s,end_s,offered_water_m3,accepted_water_m3,rejected_water_m3,scale,accepted_enthalpy_j,species.NH4_N_kg-N,species.NO3_N_kg-N
"interval-receiver","controlled-donor:1",2440587.5,0,60,2,1,1,1,50,0.10000000000000001,0.050000000000000003
```

## Status messages

The pair sets no status messages of its own; only the SDK's initialization messages, and
the error text of Invalid and Failed, appear. A one-interval run:

```text
[interval] status messages of a one-interval run
  source
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated ""
    Updating ""
    Done ""
    Finishing ""
    Finished ""
  receiver
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated ""
    Updating ""
    Done ""
    Finishing ""
    Finished ""
```

## Errors

Configuration errors, produced by `docs/examples/reference_messages`:

```text
[interval] errors
  no config
    Argument initialization failed: [json.exception.parse_error.101] parse error at line 1, column 1: attempting to parse an empty input; check that your input string or stream contains the expected JSON
  schema hydrocouple.interval-transfer/2
    Argument initialization failed: Use schema hydrocouple.interval-transfer/1
  no epoch_julian_day
    Argument initialization failed: [json.exception.out_of_range.403] key 'epoch_julian_day' not found
  species basis "g"
    Argument initialization failed: Use unique species IDs and a declared kg or kg-N basis in this reference profile
  source scale 2
    Argument initialization failed: The controllable source requires scale 1
  receiver scale -1
    Argument initialization failed: scale must be one finite nonnegative number; conservation qualification requires 1
  first interval starting at 10 s
    Argument initialization failed: Source intervals must be contiguous and increasing from zero
  interval without its species amount
    Argument initialization failed: Supply each configured species exactly once per interval
  negative water
    Argument initialization failed: Supply finite values and nonnegative water/species amounts
  dry interval carrying heat
    Argument initialization failed: A dry interval cannot transfer waterborne species or heat
  no intervals
    Argument initialization failed: Supply at least one source interval
  expected_transfers 0
    Argument initialization failed: expected_transfers must be a positive integer
  empty receipt_file
    Argument initialization failed: Supply a receipt_file and finite nonnegative max_water_m3
  config set after initialize()
    Configure before initialization; create another instance to change the port catalogue
  update() before prepare()
    Source is not ready to update
  validate() with nothing connected
    transfer_id: Use a direct scalar interval-transfer port with matching field, species basis and units
```

Every message the committed source can produce:

| Message | Where | Cause | Fix |
| --- | --- | --- | --- |
| a JSON library message (`parse error`, ``key '<k>' not found``, `type must be ...`) | `initialize()` | `config` is empty, not JSON, lacks a required key, or holds a value of the wrong type. | Supply the configuration described above. |
| `Use schema hydrocouple.interval-transfer/1` | `initialize()` | `schema` names another profile. | Set `schema` to `hydrocouple.interval-transfer/1`. |
| `config and scale must each contain exactly one value` | `initialize()` | An argument holds zero or several values. | Give each exactly one value. |
| `scale must be one finite nonnegative number; conservation qualification requires 1` | `initialize()` | `scale` is negative, infinite or NaN. | Use 1, or another finite nonnegative value for argument tests. |
| `epoch_julian_day must be finite` | `initialize()` | The epoch is infinite or NaN. | Give a finite Julian day. |
| `species must be an array` | `initialize()` | `species` is not a JSON array. | Write `"species": [ ... ]`. |
| `Use unique species IDs and a declared kg or kg-N basis in this reference profile` | `initialize()` | A species ID is empty, repeated or holds other characters, or a basis is neither `kg` nor `kg-N`. | Fix the IDs and bases. |
| `The controllable source requires scale 1` | source `initialize()` | `scale` is not 1 on the source. | Leave `scale` at 1. |
| `Supply each configured species exactly once per interval` | source `initialize()` | An interval's `species` object is missing a species or has another one. | List every configured species in every interval. |
| `Source intervals must be contiguous and increasing from zero` | source `initialize()` | The first interval does not start at 0, an interval does not start where the previous ended, or one does not end after it starts. | Make the intervals contiguous from 0. |
| `Supply finite values and nonnegative water/species amounts` | source `initialize()` | A value is infinite or NaN, or water or a species amount is negative. | Fix the value. |
| `A dry interval cannot transfer waterborne species or heat` | source `initialize()` | `water_m3` is 0 but heat or a species is not. | Give zero heat and species with zero water. |
| `Supply at least one source interval` | source `initialize()` | `intervals` is empty. | Add an interval. |
| `expected_transfers must be a positive integer` | receiver `initialize()` | `expected_transfers` is not a JSON integer, or not above 0. A missing key fails with the JSON library's message instead. | Give the number of intervals the source delivers. |
| `Supply a receipt_file and finite nonnegative max_water_m3` | receiver `initialize()` | `receipt_file` is empty, or the capacity is negative or not finite. | Fix the key. |
| `Configure before initialization; create another instance to change the port catalogue`, or `Configure before initialization` | setting `config` | `config` was set after the status left Created. The message is also queued on `errors()`. | Configure a new instance. |
| `<field>: Use a direct scalar interval-transfer port with matching field, species basis and units` | receiver `validate()`, `prepare()`, `update()`; `canConsume()` | An input is unconnected, or its provider differs in ID, description, type, shape, unit or basis, or is an adapted output. | Connect each input to the source output with the same ID, directly. |
| `Interval provider must have an owning component` | as above | A provider belongs to no component. | Connect a component's output. |
| `An interval tuple must come from one donor component` | as above | Inputs are connected to outputs of different components. | Connect every input to the same source. |
| `The interval donor is unavailable` | as above | The donor is Failed or Finished. | Rerun with a fresh donor. |
| `Connect a separate interval donor` | as above | No input is connected, or the receiver is its own donor. | Connect a source. |
| `Receiver is missing donor species: <output>` | as above | The donor publishes a species the receiver did not configure. | Configure every donor species on the receiver. |
| `Register exactly one direct receiver for every interval field` | as above | A donor output also feeds another input, or has adapted outputs. | Give each field one consumer, without adapters. |
| `Cannot open receiver receipt file: <path>` | receiver `prepare()` | The receipt file cannot be created. | Choose a writable `receipt_file`. |
| `Cannot write receiver receipt header` | receiver `prepare()` | The header write failed. | Check the disk and permissions. |
| `Source is not ready to update`, `Receiver is not ready to update` | `update()` | `update()` was called outside the Updated state; only queued, the status does not change. | Prepare first; stop updating after Done. |
| `Transfer changed during retrieval` | receiver `update()` | `transfer_id` differed before and after reading the fields. | Do not update the donor during the receiver's update. |
| `Supply a nonempty transfer ID without line breaks` | receiver `update()` | The donor published an empty ID or one with a line break. | Fix the donor. |
| `Changed or stale transfer ID` | receiver `update()` | A known ID came back with different values, or an older ID came back. | Give every interval a new ID; repeat only the latest one unchanged. |
| `Invalid water/species/heat amount` | receiver `update()` | An offered value is not finite, or water or a species is negative. | Fix the donor's values. |
| `Epoch mismatch or noncontiguous/stale transfer interval` | receiver `update()` | The epoch differs from the configuration, or the interval does not start where the last accepted one ended, or does not end after it starts. | Align the epochs and the intervals. |
| `A dry interval cannot carry waterborne species or heat` | receiver `update()` | Zero water with heat or species. | Fix the donor's values. |
| `Donor completion does not match expected_transfers` | receiver `update()` | The donor is Done before or after the `expected_transfers`-th transfer. | Make `expected_transfers` equal the number of source intervals. |
| `Receiver cumulative amount overflow` | receiver `update()` | A running total is no longer finite. | Reduce the amounts or `scale`. |
| `Receipt write failed; stop this run without retry` | receiver `update()` | The receipt row could not be written. | Fix the disk problem and rerun from the start. |

The messages from `update()` set the status to Failed, except the two "not ready" ones.

## Example composition

`compositions/interval-transfer.json` configures a pair that delivers two intervals, 5 m3
of water, 250 J, 0.5 kg-N of `NH4_N` and 0.25 kg-N of `NO3_N` in all. These are test
amounts, with no reaction or calibration behind them:

\include{lineno} interval-transfer.json

The @ref cookbook "cookbook" runner applies it with the document's IDs as instance IDs and
leaves the receipt `out/interval-receipts.csv`; the `writers` and `run` blocks are carried
out by a host such as Composer. The developer guide gives the headless command line.

The test suite drives the same intervals through the C++ interfaces. Here the receiver
lists its species in the opposite order to the source:

\dontinclude test_intervalreceiver.cpp
\skip UnequalIntervalsCloseWaterSpeciesAndHeatAndLeaveReceipts
\until }

## Tests

`IntervalReceiverTest` (19 tests) covers:

| Test | What it checks |
| --- | --- |
| `ConfiguredMetadataIsDiscoverableWithoutWritingOutputs` | 8 inputs after `initialize()`, units and value kinds, no receipt file, no checkpointing, `config` round trip |
| `UnequalIntervalsCloseWaterSpeciesAndHeatAndLeaveReceipts` | totals 5 m3, 250 J, 0.5 and 0.25 kg-N, the last ID, three receipt lines |
| `IdenticalReplayDoesNotCreditOrWriteTwice` | a repeated update credits once and writes one row |
| `ChangedReplayFailsWithoutPartialCredit` | a changed repeat fails and keeps the totals |
| `PartialAcceptanceScalesTheWholeTupleAndRetainsRejection` | capacity 1 m3 halves every amount and rejects 1 m3 |
| `ZeroCapacityRefusesWithoutCreditingInventory` | capacity 0 credits nothing and rejects 2 m3 |
| `NonConservativeScaleIsExplicitAndOnlyForArgumentTesting` | `scale` 2 doubles the totals only |
| `InvalidFieldFailsAtomically` | a NaN species fails with no credit and no row |
| `ReverseWaterIsRejectedByThisOneWayReferenceProfile` | negative water fails |
| `WrongEpochOrStaleIntervalFailsBeforeCredit` | a wrong epoch fails before credit |
| `SpeciesBasisMismatchRefusesTheConnection` | `kg` offered to `kg-N` is refused |
| `MissingDonorSpeciesIsNotSilentlyDiscarded` | a donor species without an input makes the receiver Invalid |
| `MissingArgumentFailsBeforePortsOrFilesExist` | no `config`: Failed, no ports, no file |
| `BufferShapeAndTypeAreValidated` | wrong buffer shape and kind are refused |
| `EmptyScaleArrayFailsWithoutReadingMissingStorage` | an empty `scale` fails initialization |
| `ConfigurationCannotChangeAfterCatalogueInitialization` | `config` is refused after `initialize()` |
| `StaleNewIdCannotReplayAnOldInterval` | a new ID for an old interval fails |
| `DryTransferHasZeroInventories` | a dry interval is accepted with zero totals |
| `TransferCountMismatchStopsBeforePrematureCompletion` | `expected_transfers` 1 against two intervals fails before credit |

`docs.example.cookbook_compositions` runs the composition, and
`docs.example.reference_messages` produces the status messages and errors above.
