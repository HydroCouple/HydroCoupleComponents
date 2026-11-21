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

Both libraries are built from `components/intervalreceiver/intervalcomponents.cpp`; the
source's build defines `HCC_INTERVAL_SOURCE`. The page
[Developing an interval-transfer component](../INTERVAL_TRANSFER_COMPONENTS.md) is the
developer guide to the profile, including what a custom component must test before it
claims to implement it. This page is the reference for the two components themselves.

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

The host advances the source once and then the receiver once for each interval. Reading
the source's outputs never advances it. The source is an offer generator: it does not
model a donor or debit any inventory, and what the receiver does not accept stays the
donor's responsibility.

## Arguments

| ID | Type | Default | Meaning |
| --- | --- | --- | --- |
| `config` | String holding a JSON object; required | none | The configuration below. Apply it before `initialize()`; once initialization has started it is refused, so configure a new instance to change the ports. Serializing it as JSON returns the same object. |
| `scale` | Float64 scalar; optional, Parameter role | 1 | Finite and nonnegative. On the receiver it multiplies the cumulative outputs only; the per-transfer accepted amounts stay physical. Conservation requires 1, and other values only exercise argument handling. The source refuses anything but 1. |

`initialize()` validates the configuration and creates the ports it describes. It does
not advance anything or create files.

## Configuration

Both components read:

| Key | Value |
| --- | --- |
| `schema` | `"hydrocouple.interval-transfer/1"` |
| `epoch_julian_day` | Finite Julian day that interval times count from. |
| `species` | Array of `{ "id": ..., "basis": ... }`. Each `id` is nonempty, unique, and made of letters, digits, `_`, `-` and `.`; each `basis` is `kg` or `kg-N`. |

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
| `receipt_file` | Nonempty path of the receipt CSV file. A relative path resolves against the component's reference directory. |
| `max_water_m3` | Optional finite, nonnegative capacity per offered interval. Without it there is no limit. |

`compositions/interval-transfer.json` configures a pair that delivers two intervals, 5 m3
of water, 250 J, 0.5 kg-N of `NH4_N` and 0.25 kg-N of `NO3_N` in all. These are test
amounts, with no reaction or calibration behind them:

\include{lineno} interval-transfer.json

The test suite drives the same intervals through the C++ interfaces. The composition
itself runs in a host such as Composer: its `writers` and `run` blocks are carried out by
the host, and the developer guide gives the headless command line.

## Ports

The source's outputs and the receiver's inputs are the same fields, created from the
configuration. Every item holds one host-memory value, shape `[1]`, and carries the
description `hydrocouple.interval-transfer/1;field=<field ID>`. Each numeric field has a
quantity with its unit, its SI dimensions and its value kind.

| Field ID | Type | Unit | Dimensions | Value kind |
| --- | --- | --- | --- | --- |
| `transfer_id` | String | | | |
| `epoch_julian_day` | Float64 | `day` (86400 s) | time | intensive |
| `interval_start_s`, `interval_end_s` | Float64 | `s` | time | intensive |
| `water_m3` | Float64 | `m3` | length cubed | extensive |
| `enthalpy_j` | Float64 | `J` | mass, length squared, time to the power -2 | extensive |
| `species.<id>` | Float64 | `kg` or `kg-N`, as declared | mass | extensive |

The source publishes `transfer_id` as `<instance ID>:<n>`, counting intervals from 1, and
reports Done after its last interval.

The receiver adds these outputs:

| Output ID | Contents |
| --- | --- |
| `accepted_transfer_id` | ID of the last accepted transfer. |
| `accepted.water_m3`, `accepted.enthalpy_j`, `accepted.species.<id>` | Amounts accepted from the last transfer. |
| `cumulative.water_m3`, `cumulative.enthalpy_j`, `cumulative.species.<id>` | Running totals of the accepted amounts, multiplied by `scale`. |
| `rejected_water_m3` | Water offered in the last transfer and not accepted. |

They keep the units of the fields they report.

## Connecting the pair

Connect each receiver input to the source output with the same ID. The order in which
the two configurations list their species does not matter. A receiver input accepts a
provider only when it is a direct output, not an adapted one, with the same ID,
description, data type and shape `[1]`, and, for numeric fields, the same value kind, unit
label, conversion to SI and dimensions. A field with another basis, for example `kg`
offered to a `kg-N` input, is refused with a message.

At `validate()`, at `prepare()` and on every update the receiver also checks that:

- every input is connected, and all of them to the same donor component, which is not
  the receiver itself and is neither Failed nor Finished;
- every `species.` output of the donor has a matching receiver input, so no species is
  dropped;
- each donor output feeds this receiver input alone and has no adapted outputs.

The profile therefore allows one donor and one physical receiver, with no temporal or
spatial adapters between them.

## What the receiver does with a transfer

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
- **One direction only.** Negative water would be a reversal, which this receiver does
  not support, and is rejected.

The components advertise no checkpointing or cloning capability.

## The receipt file

`prepare()` creates the receipt file and any missing parent directories, truncates it,
and writes the header. Each accepted transfer then adds one row and is flushed at once.
The columns are

`instance_id`, `transfer_id`, `epoch_julian_day`, `start_s`, `end_s`, `offered_water_m3`,
`accepted_water_m3`, `rejected_water_m3`, `scale`, `accepted_enthalpy_j`

followed by one `species.<id>_<basis>` column of accepted amounts per species, in the
receiver's configured order. IDs are quoted, and numbers are written with up to 17
significant digits. This is the receipt from
`IntervalReceiverTest.PartialAcceptanceScalesTheWholeTupleAndRetainsRejection`, where a
capacity of 1 m3 accepts half of a 2 m3 interval:

```
instance_id,transfer_id,epoch_julian_day,start_s,end_s,offered_water_m3,accepted_water_m3,rejected_water_m3,scale,accepted_enthalpy_j,species.NH4_N_kg-N,species.NO3_N_kg-N
"interval-receiver","controlled-donor:1",2440587.5,0,60,2,1,1,1,50,0.10000000000000001,0.050000000000000003
```

## A complete run in the tests

`IntervalReceiverTest` configures the source and the receiver as the composition does,
wires every receiver input to the source output with the same ID, and runs both
intervals. Here the receiver lists its species in the opposite order to the source:

\dontinclude test_intervalreceiver.cpp
\skip UnequalIntervalsCloseWaterSpeciesAndHeatAndLeaveReceipts
\until }

The other tests in the suite cover replays, partial and zero capacity, a non-unit
`scale`, invalid and reversed values, a wrong epoch, a basis mismatch, a missing species,
missing or malformed arguments and a transfer count that disagrees with the donor.
