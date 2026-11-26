# Tutorial: write, package and load a component {#tutorial_component}

[TOC]

This tutorial builds a new component library in this repository, starting from the smoke
component, and carries it the whole way: the component class and its metadata, the CMake
target, the installed package, a host that finds the library, checks its ABI stamp, loads
it and runs it in a composition, and the tests that prove each step. The component is a
moving average: it averages the last `window` values that arrive at its input.

Everything shown here is compiled and run by the test suite. The code lives in
`docs/examples/tutorial_moving_average/`:

| File | What it is |
| --- | --- |
| `movingaveragecomponent.cpp` | the component and its info class, built as the plugin `hcc_docs_movingaverage` |
| `CMakeLists.txt` | the plugin target, its install rule and the two tests |
| `main.cpp` | a small host: search, load, run, unload |
| `moving_average.json` | the composition the host runs |

It is a documentation example, not a shipped plugin: a plain `cmake --install` leaves it
out. The last section lists what a shipped component adds.

The HydroCouple SDK guide covers the base classes this builds on in depth:
[Components](https://www.hydrocouple.org/HydroCoupleSDK/guide/component.html),
[Exchange items](https://www.hydrocouple.org/HydroCoupleSDK/guide/ports.html) and
[Workflows](https://www.hydrocouple.org/HydroCoupleSDK/guide/workflow.html). Its
[first component](https://www.hydrocouple.org/HydroCoupleSDK/guide/first_component.html)
chapter builds a component inside one program; this tutorial adds the library boundary.

## 1. Start from the smoke component

The smoke component (@ref ref_smoke "reference") is the smallest complete component: a
component class derived from the SDK's `AbstractModelComponent`, an info class derived
from `AbstractModelComponentInfo` that creates instances, and the macro that publishes the
info. Copy `components/smoke/smokecomponent.cpp` to a new file and rename its two classes.
Each part of the smoke component has a counterpart below:

| Smoke component | Moving average |
| --- | --- |
| no arguments | `window`, an integer of at least 1 |
| no inputs | `in`, the value to average |
| output `produced`, set to 42 | output `average`, the mean of the last `window` inputs |
| Done after one update | Done when its provider is Done |

## 2. Includes and the unnamed namespace

\snippet tutorial_moving_average/movingaveragecomponent.cpp includes

`hydrocouplesdk/componentabi.h` brings in the loading convention of the HydroCouple
interface package: the ABI stamp and the macro `HYDROCOUPLE_DECLARE_COMPONENT`. The other
headers are the SDK base classes for the component and its info, dimensions, value
definitions (quantities), and 1-D arguments and exchange items.

Every class goes in an unnamed namespace. The classes then have internal linkage, so two
plugins may define classes with the same names, and nothing but the two entry points the
macro emits leaves the library. With the hidden symbol visibility the build sets (step 8),
the library exports exactly those two C functions.

## 3. The component class

\snippet tutorial_moving_average/movingaveragecomponent.cpp component

The constructor takes the instance ID and passes a caption to the base class. The info
class passes a default ID; a host such as Composer replaces it with the composition's
component ID through `setInstanceId()` before initialization.

## 4. Arguments

\snippet tutorial_moving_average/movingaveragecomponent.cpp arguments

The base class calls `createArguments()` the first time anyone asks for `arguments()`,
which the interfaces allow in the Created state: a host lists and sets arguments before
`initialize()`. `createArguments()` creates each argument in a `std::unique_ptr` member,
gives it its default and registers it with `addArgument()`. Registering only lists the
item: `AbstractModelComponent` never deletes what it lists, so the component must own it.

`initialize()` calls `initializeArguments()`. This is where the component reads the
argument values into members ("latches" them) and refuses values it cannot use, by
returning false with a message. The SDK then sets the status to Failed with the message
`Argument initialization failed: ` followed by yours, queues it on `errors()`, and calls
`initializeFailureCleanUp()`.

Check the number of values first. A composition sets an argument with a JSON array of any
length, `{ "values": [] }` included, and `operator[]` on an SDK item is bounds-checked: on an
empty argument it throws `std::out_of_range`, which leaves `initialize()` as an exception
instead of a message. The shipped testing components do not make this check (see
@ref ref_signalsource "Signal Source"); new components should.

## 5. Exchange items

\snippet tutorial_moving_average/movingaveragecomponent.cpp ports

`createInputs()` and `createOutputs()` run in `initialize()`, after the arguments were
accepted, so the ports can depend on them (the time series provider makes one output per
series of its source). Each port is created in a `std::unique_ptr` member and registered
with `addInput()` or `addOutput()`. `initialize()` empties the base class's lists before it
calls these functions, so on a second `initialize()` the assignment frees the previous port
and the list holds only the new one. An `Input1DDouble` accepts any rank-1 Float64 provider
and, at `applyInputValues()`, copies as many of the provider's leading values as it holds,
here one. `initializeFailureCleanUp()` releases anything a failed initialization left
behind; the members already own everything here.

Every `initialize()` recreates the ports, which drops the connections made to the old
ones. Hosts therefore set arguments, initialize, and only then connect.

## 6. The lifecycle

\snippet tutorial_moving_average/movingaveragecomponent.cpp validate

`validate()` runs after the connections are made. It returns the problems it finds and
ends at Valid or Invalid; this component has nothing to check.

\snippet tutorial_moving_average/movingaveragecomponent.cpp prepare

`prepare()` resets the state a run accumulates, so preparing again restarts the run. It
ends at Updated, the state `update()` expects.

\snippet tutorial_moving_average/movingaveragecomponent.cpp update

`update()` advances one step. `applyInputValues()` pulls the provider's current value into
`in`. The component then reports Done once its provider's component is Done or Finished,
and Updated before that: a consumer keeps to its provider's clock, the rule the gain
transformer, the recorder and the combiner follow. Every status change goes through
`setStatus()`, which emits the status event hosts listen to; the second argument is the
message they show. The status must follow the interfaces' transition table, for example
Updated, Updating, then Updated or Done: the SDK queues `illegal status transition` on
`errors()` for a change the table rejects.

\snippet tutorial_moving_average/movingaveragecomponent.cpp finish

`finish()` releases what the run holds, such as open files, and ends at Finished.

## 7. Members, ownership and the info class

\snippet tutorial_moving_average/movingaveragecomponent.cpp members

The component owns everything it creates. The dimension, the unit and the quantities come
first: members are destroyed in reverse order of declaration, so what the items point to
outlives the items. A `Quantity` does not own its `Unit`, so the component owns that too;
the SDK's shortcut `Quantity::unitLess()` allocates a unit that nothing frees, which is why
this component builds its quantities from its own `Unit::unitlessCoefficient()`. The
argument and the ports follow, each in a `std::unique_ptr`; the base class's lists hold
only non-owning pointers to them.

The shipped plugins do not follow this pattern yet: they create their arguments and ports
with `new` and only register them, so each instance's items, and the ports of every
earlier `initialize()`, are never freed. New components should own their items as this
one does.

\snippet tutorial_moving_average/movingaveragecomponent.cpp info

The info class carries the metadata hosts show before any instance exists: the component
info ID, which compositions name in `info.component_info_id`, the caption, description,
developer, version and tags. `createComponentInstance()` returns a new instance that the
caller owns. Give the info ID a namespace you control; IDs must be unique among the
libraries a host loads, and Composer refuses a second library with the same ID.

\snippet tutorial_moving_average/movingaveragecomponent.cpp declare

The macro, once in exactly one source file of the library, emits the two exported C
functions: `hydrocouple_component_abi_v1()`, which returns the ABI stamp, and
`hydrocouple_component_info_v1()`, which returns the info, a function-local static the
library owns. @ref concepts "Concepts" explains both.

## 8. Build the library

\snippet tutorial_moving_average/CMakeLists.txt plugin

A component library is a CMake `MODULE`: a library meant to be loaded at run time with
`dlopen`, never linked into a program. It links HydroCoupleSDK privately. The root
`CMakeLists.txt` sets hidden symbol visibility for every target, so only the two entry
points, which the ABI header marks visible, are exported. CMake names a `MODULE` library
`lib<target>.so` on Linux and on macOS. The library lands in
`build/docs-examples/components/`:

```sh
cmake --build build --target hcc_docs_movingaverage
nm -gU build/docs-examples/components/libhcc_docs_movingaverage.so
```

```text
0000000000001250 T _hydrocouple_component_abi_v1
000000000000125c T _hydrocouple_component_info_v1
```

A shipped component instead gets its own directory, `components/<name>/`, with a
`CMakeLists.txt` of one line, and an `add_subdirectory(components/<name>)` line in the root
`CMakeLists.txt`:

```cmake
hcc_add_component(hcc_<name> <name>component.cpp)
```

`hcc_add_component` (defined in the root `CMakeLists.txt`) makes the same `MODULE`, links
HydroCoupleSDK, writes the library to `build/components` and installs it to
`lib/hydrocouple/components`.

## 9. Package it

\snippet tutorial_moving_average/CMakeLists.txt install

Installing puts the library in `lib/hydrocouple/components` below the prefix, the
directory hosts search. The tutorial's rule belongs to the install component
`hcc_docs_examples` and is excluded from a plain install, so it is installed only on
request:

```sh
cmake --install build --component hcc_docs_examples --prefix <stage>
```

A shipped component is installed with the rest by `cmake --install build --prefix <prefix>`.

The installed library carries no runtime search path. On macOS `otool -L` shows that it
needs `@rpath/libHydroCoupleSDK.dylib`, and `otool -l` shows no `LC_RPATH` entry: it uses
the HydroCoupleSDK library the host has already loaded. A host must therefore link, or load,
the same HydroCoupleSDK build the plugin was built against, and must itself be able to find
that library's dependencies (@ref troubleshooting "Troubleshooting" shows the failures when
it cannot).

## 10. Load it in a host

A host finds component libraries by searching directories. Composer searches, in order: on
macOS its bundle's `PlugIns/HydroCouple`; a `components` directory beside its executable;
`lib/hydrocouple/components` in its installation prefix; every directory in the
`HYDROCOUPLE_COMPONENT_PATH` environment variable (separated by `:`, `;` on Windows); and
the directories added in its component settings. It considers files ending in `.dylib`,
`.so` or `.bundle` on macOS, `.so` on Linux and `.dll` on Windows.

The tutorial's host takes its directories on the command line. Loading one library:

\snippet tutorial_moving_average/main.cpp load

The order is the safety rule of the loading convention. `dlopen` with `RTLD_NOW |
RTLD_LOCAL` resolves every symbol at once and keeps the library's symbols out of the global
namespace. The stamp function is plain C returning a string, so it is safe to call even
when the library was built with another toolchain; it is the only function a host may call
before the stamps match, because a C++ call across a mismatch has no recovery. Only after
the comparison does the host call the info function and use C++ objects.

Searching a directory:

\snippet tutorial_moving_average/main.cpp search

Running a composition with the loaded components: one instance per component block, named
after the block, its reference directory set, then `ModelInitializer` and a time-stepped
workflow, as `CompositionsTest` and Composer do:

\snippet tutorial_moving_average/main.cpp run

The composition puts the moving average between a ramp and a recorder:

\include{lineno} tutorial_moving_average/moving_average.json

Unloading has one rule: destroy every instance a library created before closing that
library, because the instance's code and virtual tables live in it. The host keeps its
instances in a scope narrower than its libraries' and closes the libraries last, in
reverse order of loading:

\dontinclude tutorial_moving_average/main.cpp
\skip Every instance is gone
\until dlclose(library->handle);

## 11. Test it

The example's `CMakeLists.txt` registers two tests. The first installs the plugin into a
staging prefix in the build tree; the second, which requires it as a fixture, runs the host
over the staged directory and the build's shipped plugins:

\snippet tutorial_moving_average/CMakeLists.txt tests

`hcc_add_docs_example` (in `docs/examples/CMakeLists.txt`) builds the host, links
HydroCoupleSDK, and registers the test `docs.example.<name>` with the label `docs-example`,
running in `build/docs-examples/<name>/`. Run both:

```sh
ctest --test-dir build -R tutorial_moving_average --output-on-failure -V
```

The host's output:

```text
host stamp: hc-abi/2;iface=5;cxx=clang-21;stdlib=libc++1;ptr=8

loaded  libhcc_docs_movingaverage.so -> hydrocouple.components.docs.moving_average
loaded  libhcc_basicadapters.so -> hydrocouple.components.adapters.basic
loaded  libhcc_combiner.so -> hydrocouple.components.combiner
loaded  libhcc_gaintransformer.so -> hydrocouple.components.gain
loaded  libhcc_intervalreceiver.so -> org.hydrocouple.openswmm.test.sink
loaded  libhcc_intervalsource.so -> org.hydrocouple.openswmm.test.interval-source
loaded  libhcc_meshgenerator.so -> hydrocouple.components.meshgenerator
loaded  libhcc_recorder.so -> hydrocouple.components.recorder
loaded  libhcc_signalsource.so -> hydrocouple.components.signal_source
loaded  libhcc_smoke.so -> hydrocouple.components.smoke
loaded  libhcc_timeseriesprovider.so -> hydrocouple.components.timeseriesprovider

moving_average.csv:
step,value
1,2
2,3
3,5
4,3

with window 0:
Component "average" failed to reach Initialized after initialize()
  - Argument initialization failed: window must be at least 1, not 0

OK
```

The ramp of amplitude 8 at a quarter cycle per step gives 2, 4, 6 and 0; the averages over
two steps are 2, 3, 5 and 3. The second run shows a refused argument the way a host sees
it: `ModelInitializer` names the component and lists its `errors()`. The stamp line depends
on the compiler: it names the convention's revision, the interface ABI, the compiler family
and major version, the standard library and the pointer size.

## 12. Make it a shipped component

To turn such a component into one this repository ships:

1. Put the source in `components/<name>/` with its one-line `CMakeLists.txt`, and add
   `add_subdirectory(components/<name>)` to the root `CMakeLists.txt`.
2. Add a test file to `tests/`, list it in `tests/CMakeLists.txt` and add the new target to
   the test executable's `add_dependencies`, so the plugin is built before the tests run.
   Load the plugin with `dlopen` the way the existing suites do, from `HCC_COMPONENTS_DIR`.
3. Add an entry for the source file to `DESCRIPTIONS` in `docs/namespace_docs_filter.py`.
   The documentation build documents each plugin's unnamed namespace from that table, and
   a source file without an entry produces a warning, which fails the documentation
   workflow.
4. Write a reference page on the template of the @ref component_reference "component
   reference", and add it to `INPUT` in `docs/Doxyfile` and as a `@subpage` of its family
   page.
5. Document every class, member and parameter in the source; the reference is built from
   those comments.
