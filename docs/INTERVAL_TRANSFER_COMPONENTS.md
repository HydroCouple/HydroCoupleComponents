# Developing an interval-transfer component

The CF2 reference pair is a working starting point for custom components:
`hcc_intervalsource` publishes controlled offers and `hcc_intervalreceiver`
receives them. Their IDs are `org.hydrocouple.openswmm.test.interval-source`
and `org.hydrocouple.openswmm.test.sink`. These implement the versioned
OpenSWMM reference profile `hydrocouple.interval-transfer/1`; this profile
adds conventions on top of the existing HydroCouple ABI, without changing
its virtual interfaces. It is not a general hydraulic solver or a replacement
for the later OpenSWMM engine wrapper.

## Build and load

Build this repository against a compatible installed HydroCoupleSDK and its
dependencies. Targets `hcc_intervalsource` and `hcc_intervalreceiver` create
stamped shared libraries in `build/components` (or the equivalent chosen build
directory). They use the SDK; OpenSWMM's own engine and wrappers remain SDK-free.
Install the modules under `lib/hydrocouple/components`, or supply their directory
through Composer's component settings or `HYDROCOUPLE_COMPONENT_PATH`.

Open `compositions/interval-transfer.json` in Composer. It is also executable
with `HydroCoupleComposer --run <composition.json> --components <plugin-directory>`.
The example asks for a receipt CSV, recorded outputs and a run manifest below
`out/`, relative to the composition. Use a copy in a reviewable run directory.
Its two intervals deliver 5 m3, 250 J, 0.5 kg-N of NH4_N and 0.25 kg-N of NO3_N.
These are test amounts, with no reaction or field-calibration claim.

## Configuration and discovery

`config` accepts a JSON object with schema `hydrocouple.interval-transfer/1`,
a finite `epoch_julian_day`, and a `species` array. Each species declares a
unique stable `id` and a `basis`: this initial profile accepts `kg` and `kg-N`.
The source additionally declares `intervals`; the receiver declares a positive
integer `expected_transfers`, an explicit `receipt_file` and optionally
`max_water_m3`, a finite nonnegative acceptance capacity per offered interval.

Each source interval has `start_s`, `end_s`, `water_m3`, `enthalpy_j` and a
`species` object containing every configured species amount. Intervals are
contiguous and increase from zero. Relative enthalpy may be negative; water
and species amounts are nonnegative. Zero water must carry zero species/heat.

`scale` is a scalar optional parameter with default 1. On the receiver it
multiplies cumulative diagnostics, while per-transfer accepted amounts remain
physical amounts. Conservation qualification requires scale 1. Other scales
exercise configuration only. The source requires scale 1.

Apply arguments before `initialize()`. Initialization builds the actual port
catalog and validates configuration; it does not advance a model or create
output files. Configure another instance to change the catalog. Serialization
of `config` returns the same JSON object. Composer's edit-time instance follows
the same path and is separate from the run's instance.

## Typed port contract

All fields are host-memory, rank-one scalars of shape `[1]`. `transfer_id` is
String; every other input field is Float64. Every input/output carries description
`hydrocouple.interval-transfer/1;field=<field-id>`. Each numeric field has an
IQuantity with its declared SI dimensions, conversion and value kind.

| Input/source output | Meaning and units |
| --- | --- |
| `transfer_id` | Nonempty unique transaction identity; the source uses `<instance-id>:<sequence>` |
| `epoch_julian_day` | Shared Julian-day epoch, days; intensive coordinate |
| `interval_start_s`, `interval_end_s` | Bounds in seconds elapsed since that epoch; intensive coordinates |
| `water_m3` | Offered interval water volume in m3; extensive amount |
| `enthalpy_j` | Offered interval enthalpy in J on the agreed reference basis; extensive amount |
| `species.<stable-id>` | Offered interval amount in the declared kg or kg-N basis; extensive amount |

Connect fields by ID; species declaration order can differ. The receiver checks
IDs, shape, type, dimensions, unit labels/conversions and profile metadata. Every
field must come directly from the same donor component. Both sides must carry
the same species set. Unavailable mappings, temporal/spatial adapters and multiple
physical receivers are rejected by this initial profile. Reading outputs never
advances the source. The host advances the source once, then updates the receiver
once for the published tuple. The finite source must reach Done on the receiver's
configured final transfer; mismatched completion is diagnosed before credit. This reference uses the existing time-stepped
workflow; it does not implement a general adaptive coupling schedule.

A new tuple starts at the previous accepted tuple's end, with matching epoch.
The receiver preflights all fields and capacity, logs the receipt, and then
publishes one accepted tuple. If capacity is smaller than offered water, water,
heat and every species are accepted at the same fraction. The rest remains the
donor's responsibility. The reference source is an offer generator; it does not
model or debit a physical donor inventory. Integrating a real donor requires its
accepted-receipt/debit contract and combined budgets in CF3/CF4.

Receiver outputs include `accepted_transfer_id`, `accepted.<amount-field>`,
`rejected_water_m3` and `cumulative.<amount-field>`. They retain the declared
quantity units. An identical repeat of the most recent tuple retains its
acknowledgment without a second credit or CSV row. Changed, stale or noncontiguous
transfers fail before credit. A receipt write failure stops the run; retry and
restart are not supported by this profile. Its capability set does not advertise
checkpointing or cloning. Negative water represents an unsupported reversal in
this one-way receiver and is rejected explicitly; bidirectional hydraulic
acceptance is a CF4 obligation.

## Creating your own component

Use `components/intervalreceiver/intervalcomponents.cpp` as the executable
reference for metadata, configuration, explicit data-item ownership, validation,
errors, cleanup and stamped publication. Its two exported factory entry points
come from `HYDROCOUPLE_DECLARE_COMPONENT`. Give your component type a unique ID;
let the host assign each configured instance a separate stable ID. SDK components
use the SDK setter. SDK-free OpenSWMM components expose a scalar String argument
`hydrocouple.instance_id`, accepting String and JSON `{ "values": ["id"] }` before
initialization, then reflecting that identity through `id()` and transfer IDs.
The argument uses the Configuration role. Assignment must reject empty IDs and
conflicting reassignment. Hosts verify that `id()` matches the document identity.
This argument convention requires no new virtual interface or core ABI version.

Before integrating a new model, test its lifecycle, configured port discovery,
argument round trips, wrong-unit/basis/shape refusal, repeated delivery, partial
acceptance, dry values, errors and independent instances. Add budgets for every
inventory the component owns. Save test inputs and receipts in a reviewable
repository directory. Qualify restart and additional spatial/temporal mappings
before advertising those capabilities. Ordinary OpenSWMM 1D MSX species are
reported through its `.out` pollutant columns; custom component diagnostics use
their own declared outputs and run manifests.
