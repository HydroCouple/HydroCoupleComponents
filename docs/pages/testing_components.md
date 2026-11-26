# Testing components {#testing_components}

[TOC]

The testing family is a set of small components whose results follow exactly from their
arguments, so a composition built from them has a known answer. Hosts and end-to-end
tests use them to check wiring, argument bindings, multi-input roles and adapter chains
with exact values. None of them models anything physical, and all their quantities are
unitless.

| Component | Library | Ports | Page |
| --- | --- | --- | --- |
| Signal Source | `hcc_signalsource` | output `signal` | @subpage ref_signalsource "Signal Source" |
| Gain Transformer | `hcc_gaintransformer` | input `in`, output `out` | @subpage ref_gaintransformer "Gain Transformer" |
| Recorder | `hcc_recorder` | input `in` | @subpage ref_recorder "Recorder" |
| Combiner | `hcc_combiner` | multi-input `operands`, output `combined` | @subpage ref_combiner "Combiner" |
| Basic Adapters | `hcc_basicadapters` | adapted outputs `negate`, `clamp` | @subpage ref_basicadapters "Basic Adapters" |

## What the family shares

- All five are version 1.0.0 and tagged Test. Each model component's default instance ID
  is its info ID followed by `.instance`; the factory's is its info ID.
- Every port is a rank-1 Float64 item of length 1 on the dimension `scalar`, with two
  exceptions: the combiner's `operands` holds one value per connected provider, and the
  adapters accept a rank-1 Float64 output of any length.
- The inputs accept any rank-1 Float64 provider, whatever its quantity, and read its first
  value. Nothing compares units.
- The consumers, the gain transformer, the recorder and the combiner, report Done once
  every connected provider's component is Done or Finished, and Updated before that. The
  signal source decides when a run ends.

## Compositions built from the family

| Document | What it shows | Recorded |
| --- | --- | --- |
| `source_to_recorder.json` | a direct connection | 5, 5, 5 |
| `source_gain_recorder_adapted.json` | an adapter chain from a standalone factory | −5, −7, −9 |
| `combiner_roles.json` | providers connected by role | 7, 7 |
| `bound_arguments.json` | an argument bound to another component's output | 105, 105 |

The @ref cookbook "cookbook" explains each one and shows its run.
