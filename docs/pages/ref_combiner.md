# Combiner {#ref_combiner}

[TOC]

The combiner sums, multiplies or averages the values of the providers connected to its
multi-input, once per update. It is the repository's only multi-input component: its input
takes several providers, each under a named role, so it exercises the role machinery of
the interfaces (`IMultiInput`), of `ModelInitializer` and of a composition's `to.role`
field.

| | |
| --- | --- |
| Library | `hcc_combiner`, built as `build/components/libhcc_combiner.so` |
| Component info ID | `hydrocouple.components.combiner` |
| Caption, version, tags | Combiner; 1.0.0; Test, Transformer |
| Default instance ID | `hydrocouple.components.combiner.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent`; input: `IMultiInput` |
| Source | `components/combiner/combinercomponent.cpp` |

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `operation` | String | `sum` | none | `sum`, `product`, `mean` (case-sensitive) | The combination. With no operands the sum and the mean are 0 and the product is 1. |

The argument carries a Quality listing the three names for hosts that offer choices.

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Multi-input | `operands` | "Operand", unitless; Float64, shape `[n]`, n the operands read at the last update | none | the providers' values at this update |
| Output | `combined` | "Combined", unitless; Float64, shape `[1]` | none: one value on the dimension `scalar` | the value of the last update; no time stamp |

### Provider roles

`operands` declares two roles, both required:

| Role ID | Caption |
| --- | --- |
| `augend` | Augend |
| `addend` | Addend |

A provider is accepted only under one of them, and only when it is a rank-1 Float64
output; only its first value is read. In a composition, the connection names the role in
`to.role`:

\dontinclude combiner_roles.json
\skip "connections"
\until "role": "addend" } },

`operands` cannot be written (`setValuesFrom()` refuses), and its shape counts the values
read at the last update, not the providers connected.

## Behavior

### initialize()

Latches `operation` and refuses any other value (see Errors), then creates `operands` with
its roles and `combined` (0).

### validate()

Moves to Valid. It does not check that both roles are filled, although the input declares
them required; a combiner with one provider, or none, runs.

### prepare()

Moves to Updated with the message `prepared`.

### update()

1. Reads the first value of every connected provider, in the order they were added. A
   provider whose read fails is skipped.
2. Writes the sum, the product, or the mean (the sum over the count) to `combined`.
3. Reports Done with the message `combined` once every connected provider's component is
   Done or Finished; Updated before that. With nothing connected it is Done after one
   update.

### finish()

Moves through Finishing to Finished.

## Status messages

A one-step run with nothing connected:

```text
[combiner] status messages of a one-step run
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Updating ""
    Done "combined"
    Finishing ""
    Finished ""
```

## Errors

```text
[combiner] errors
  operation "max"
    Argument initialization failed: operation "max" is not one of sum, product, mean
  canConsume without a role argument
    the combiner takes providers by role: augend or addend
  canConsume with no role
    name a provider role: augend or addend
  canConsume with role "multiplicand"
    "multiplicand" is not a provider role here; the roles are augend and addend
  canConsume of a String output
    the augend provider must be a rank-1 Double output
  setValuesFrom
    the operands are pulled from providers, not set
  composition connection without to.role
    connections[0]: Output "signal" cannot be consumed by input "operands": name a provider role: augend or addend
  composition connection with to.role "multiplicand"
    connections[0]: input "operands" has no provider role "multiplicand"
```

| Message | Cause | Fix |
| --- | --- | --- |
| `Argument initialization failed: operation "<o>" is not one of sum, product, mean` | `operation` holds another value. | Use `sum`, `product` or `mean`. |
| `the combiner takes providers by role: augend or addend` | A host called the role-less `canConsume(provider, message)`. | Offer the provider with a role: `canConsume(provider, message, role)` and `addProvider(provider, role)`. |
| `name a provider role: augend or addend`; in a composition, `connections[<i>]: Output "<o>" cannot be consumed by input "operands": name a provider role: augend or addend` | The connection has no role. | Add `"role": "augend"` or `"role": "addend"` to the connection's `to` block. |
| `"<r>" is not a provider role here; the roles are augend and addend`; in a composition, `connections[<i>]: input "operands" has no provider role "<r>"` | The role is neither of the two. | Use one of the two role IDs, spelled exactly. |
| `the <role> provider must be a rank-1 Double output` | The provider is not Float64 (a String output, for example) or not rank 1 (the mesh generator's `mesh`). | Connect a rank-1 Float64 output, or put an adapter between them. |
| `the operands are pulled from providers, not set` | Code tried to write the multi-input. | Connect providers instead. |

## Example composition

`compositions/combiner_roles.json` sums two constant sources, 3 and 4:

\include{lineno} combiner_roles.json

The recording is `7` at each of the two steps; see the @ref cookbook "cookbook".

## Tests

| Test | What it checks |
| --- | --- |
| `CombinerTest.CombinerRefusesAProviderForAnUnknownRole` | the roles are discoverable and required; a role-less offer and an undeclared role are refused with messages naming the roles; `augend` is accepted |
| `CombinerTest.CombinerBindsProvidersByRoleAndCombinesThem` | 3 + 4 = 7 with Updated, then Done with the providers; the mean of 3 and 4 is 3.5 |
| `CompositionsTest.CombinerCompositionSumsItsRoleProviders` | `combiner_roles.json` records 7, 7 |
| `docs.example.reference_messages` | the status messages and errors above |
