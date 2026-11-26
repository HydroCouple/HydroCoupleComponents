# Troubleshooting {#troubleshooting}

[TOC]

This page collects the failures a component author or an integrator meets with these
plugins, with the exact messages. Messages from a host are Composer's, read from its
component loader (`src/plugins/componentlibrary.cpp` and `componentregistry.cpp` in
HydroCoupleComposer); messages from the SDK and the components were produced by running
them: `docs/examples/reference_messages` prints them on every test run. For the errors
of one component, see its page in the @ref component_reference "component reference".

## A plugin does not load

A host loads a plugin in four steps: open the library, call the stamp function, compare
the stamp with its own, then call the info function. A failure at each step has its own
message.

### The library will not open

The operating system's loader refused the file; Composer reports ``cannot load '<path>':``
followed by the loader's text (`dlerror()` on Linux and macOS, a Windows error number on
Windows). The common causes:

**The plugin's HydroCoupleSDK library is not found.** An installed plugin carries no
runtime search path of its own (see @ref tutorial_component "the tutorial", step 9): it
expects the host to have loaded HydroCoupleSDK already. Loaded by a process that has not,
here a Python interpreter, it fails:

```text
dlopen(.../stage/lib/hydrocouple/components/libhcc_docs_movingaverage.so, 0x0006): Library not loaded: @rpath/libHydroCoupleSDK.dylib
  Referenced from: <9D262A4A-...> .../libhcc_docs_movingaverage.so
  Reason: tried: '.../lib-dynload/../../libHydroCoupleSDK.dylib' (no such file), '.../bin/../lib/libHydroCoupleSDK.dylib' (no such file)
```

Fix: load plugins from a host linked against the same HydroCoupleSDK, or put that SDK's
`lib` directory on the loader's path for the host process.

**The SDK's own dependencies are not found.** The installed HydroCoupleSDK library has no
runtime search path either. When vcpkg built its dependencies as shared libraries (a
dynamic triplet such as `arm64-osx-dynamic`), the executable must supply the directory that
holds them, or the process stops before `main()`:

```text
dyld[77197]: Library not loaded: @rpath/libz.1.3.2.dylib
  Referenced from: <FB8D9DCC-...> .../sdk-install/lib/libHydroCoupleSDK.dylib
  Reason: tried: '.../sdk-install/lib/libz.1.3.2.dylib' (no such file), ...
```

Fix: give the host a runtime path to `<vcpkg_installed>/<triplet>/lib`. This repository's
test executable gets one by linking GoogleTest from that tree, and `docs/examples/CMakeLists.txt`
adds it to the example hosts' `BUILD_RPATH`. A static triplet avoids the problem.

**The plugin was built against another build of HydroCoupleSDK.** The loader finds the SDK
library the host loaded, but a symbol the plugin needs is not in it, because the plugin was
compiled against different SDK headers. This came from FVQual's plugin built in October
against an older SDK, loaded beside the current one:

```text
dlopen(.../FVQual/build/components/libfvqual-component.so, 0x0006): Symbol not found: __ZTv0_n224_N11HydroCouple3SDK14AbstractOutput11addConsumerEPNS_6IInputE
  Referenced from: <040E09C4-...> .../FVQual/build/src/libfvqual-core.0.1.0.dylib
  Expected in:     <FB8D9DCC-...> .../sdk-install/lib/libHydroCoupleSDK.dylib
```

The ABI stamp does not catch this: both sides have the same compiler and interface
version, so the stamps match, but the SDK's own classes changed between the builds. Fix:
rebuild the plugin against the SDK the host uses. With the plugin rebuilt against the same
SDK, the three `DemoTest` tests pass. When `DemoTest`'s suite setup fails this way, ctest
reports the three tests as skipped, not failed; read the test's output.

**The file is not a library.** A directory can hold other files. Composer considers only
files with a library suffix (`.dylib`, `.so` and `.bundle` on macOS, `.so` on Linux, `.dll`
on Windows), and reports every other failure in its list of load failures without stopping
the scan.

### The library is not a component library

| Composer's message | Cause | Fix |
| --- | --- | --- |
| ``no such file: '<path>'`` | The path does not name a file. | Check the path. |
| `no such directory` | A search directory does not exist. | Fix `HYDROCOUPLE_COMPONENT_PATH` or the component settings. |
| ``'<path>' is not a HydroCouple component library (no hydrocouple_component_abi_v1 and no CreateComponentInfo)`` | The library exports neither entry point: it is not a component, or it was built without `HYDROCOUPLE_DECLARE_COMPONENT`, or the macro's symbols were hidden. | Add `HYDROCOUPLE_DECLARE_COMPONENT(<InfoType>)` once to the library's sources; `nm -gU` must list both `hydrocouple_component_abi_v1` and `hydrocouple_component_info_v1`. |
| ``'<path>' reported a null ABI stamp`` | The stamp function returned null. | Use the macro instead of a hand-written stamp function. |
| ``'<path>' has a valid ABI stamp but no hydrocouple_component_info_v1`` | Only the stamp function is exported. | Use the macro, which emits both. |
| ``'<path>' returned no component info`` | The info function returned null. | Use the macro. |
| ``'<path>' has no CreateComponentInfo``, ``'<path>' returned no component info from CreateComponentInfo`` | An unstamped library of the older Python-loader convention is incomplete. | Rebuild it with the stamped convention. |
| ``refused: duplicate component id '<id>' in '<path>'; already loaded from '<other path>'`` | Two libraries publish the same component info ID, for example a build tree and an installed copy on two search paths. | Remove one directory from the search paths, or give the component a unique ID. |

Composer also loads unstamped libraries that export the older `CreateComponentInfo` factory
used by HydroCouple's Python loader, and marks them as unstamped: there is no way to check
their toolchain before calling into them.

## The ABI stamps differ

The stamp is a string both the host and the library build from their own compiler settings.
Composer refuses a library whose stamp differs from its own, before calling any C++ function
in it:

```text
'<path>' was built against an incompatible toolchain or interface version.
  library expects: <library stamp>
  host provides: <host stamp>
```

A stamp has six fields; this is the one every plugin built for this manual carries:

```text
hc-abi/2;iface=5;cxx=clang-21;stdlib=libc++1;ptr=8
```

| Field | Meaning | Changes when |
| --- | --- | --- |
| `hc-abi/2` | revision of the loading convention | the convention header changes |
| `iface=5` | HydroCouple interface ABI version | the plugin and the host were built against different HydroCouple interface headers |
| `cxx=clang-21` | compiler family and major version (`gcc-<n>`; on Windows `msvc-v14x` for every Visual Studio 2015 or newer) | the compiler's major version differs |
| `stdlib=libc++1` | standard library and its ABI setting (`libstdc++-cxx11`, `libstdc++-cow`, a `-debug` suffix for libstdc++ debug mode, `msvcstl-idl<n>`) | libc++ against libstdc++, the libstdc++ dual ABI, debug mode, or the MSVC iterator-debug level differs |
| `;crt=md` | Windows only: the C runtime (`md`, `mdd`, `mt`, `mtd`) | debug against release runtime, or a static runtime |
| `ptr=8` | pointer size in bytes | 32-bit against 64-bit |

Read a library's stamp without loading it:

```sh
strings build/components/libhcc_smoke.so | grep hc-abi
```

Fix: rebuild the plugin with the host's compiler and standard library, against the same
HydroCouple interface headers as the host's SDK. Matching stamps are necessary, not
sufficient: the plugin must also be built against the same HydroCoupleSDK build as the
host (previous section).

## A connection is refused

`ModelInitializer` applies a document's `connections` after every component has
initialized, and names the connection by its index in the array. The messages:

```text
[compositions] errors
  unknown component in a connection
    connections[0].to.component "solver" is not declared in components[]
  unknown output
    connections[0]: component "hydrocouple.components.meshgenerator.instance" has no output "grid"
  unknown input
    connections[0]: component "hydrocouple.components.gain.instance" has no input "input"
  geometry output to a scalar input
    connections[0]: Output "mesh" cannot be consumed by input "in": Provider is not one-dimensional
  String output to a Float64 input
    connections[0]: Output "transfer_id" cannot be consumed by input "in": Provider data kind mismatch
  to.role on an input that is not a multi-input
    applied
```

| Message | Cause | Fix |
| --- | --- | --- |
| `connections[<i>].to.component "<c>" is not declared in components[]` (or `from.component`) | The connection names a component the document does not declare. | Declare the component, or fix the ID. |
| `connections[<i>]: component "<instance>" has no output "<o>"` / `has no input "<i>"` | The port does not exist on the initialized component. The message names the instance ID, which is the document ID only when the host assigned it. | Check the port ID on the component's reference page; for the time series provider, outputs are named after the source's series. |
| `connections[<i>]: Output "<o>" cannot be consumed by input "<i>": Provider data kind mismatch` | The SDK's 1-D input takes only its own data kind: a String output into a Float64 input. | Connect a Float64 output. |
| `... : Provider is not one-dimensional` | The output is not rank 1, such as the mesh generator's geometry-only `mesh`. | Bind the mesh to a mesh argument instead of connecting it. |
| `... : name a provider role: augend or addend`, `connections[<i>]: input "operands" has no provider role "<r>"` | The combiner's multi-input needs a role (@ref ref_combiner "Combiner"). | Add `to.role`. |
| `... : Use a direct scalar interval-transfer port with matching field, species basis and units` | An interval receiver input refuses the provider (@ref interval_transfer "Interval transfer"). | Connect each input to the same-named source output, directly. |
| `connections[<i>]: no adapted output "<id>" is offered for output "<o>" (searched ...)` | No installed factory offers the adapted output for that output (@ref ref_basicadapters "Basic Adapters"). | Install the factory; check the output's kind and shape. |

Connections compare data kinds and ranks only. Units, quantities and lengths are not
compared: a rank-1 Float64 output of any length and unit connects to every testing input,
which reads its first value. A `to.role` on an input that is not a multi-input is ignored
without a message, as the last case above shows.

## An argument is refused

```text
  unknown argument
    Component "gain" has no argument "scale"
  argument of the wrong type
    Component "gain", argument "gain": [json.exception.type_error.302] type must be number, but is string
  component that fails to initialize
    Component "sink" failed to reach Initialized after initialize()
      - Argument initialization failed: path is required: the recorder exists to leave a file behind
  binding to an output the provider lacks
    Component "gain", argument "bias": provider "source" has no output "value"
  binding to an output of the wrong shape
    Component "gain", argument "bias" rejected the bound output "mesh": Source data item is not one-dimensional
  binding to a provider that fails validation
    Component "gain", argument "bias": provider "meshgen" failed validation:
      - quad_quality must lie in [0, 1]; 0 keeps triangles
  binding to a component the document does not declare
    components["gain"].arguments.bias.@from.component "calibration" is not declared in components[]
```

| Message | Cause | Fix |
| --- | --- | --- |
| `Component "<c>" has no argument "<a>"` | The document sets an argument the component does not have. Arguments are checked against the live instance before anything is applied. | Use the IDs on the component's reference page. |
| `Component "<c>", argument "<a>": [json.exception.type_error.302] type must be number, but is string` | A value of the wrong JSON type. | Write numbers without quotes and strings with them, inside `"values": [...]`. |
| `Component "<c>" failed to reach Initialized after initialize()`, followed by the component's `errors()` | The component refused its arguments. Each line after it is one queued message, usually `Argument initialization failed: ` and the component's reason. | Fix the argument the reason names; the component pages list every reason. |
| `JSON parse error: vector` | An argument holds no values, `{ "values": [] }`, and the component's element access threw; `ModelInitializer::initializeFromJsonText` reports any exception this way. The document itself parsed. | Give each argument one value. |
| `Component "<c>", argument "<a>": provider "<p>" has no output "<o>"` | An `@from` binding names an output the provider does not have after its run. | Check the output ID. |
| `Component "<c>", argument "<a>" rejected the bound output "<o>": <reason>` | The argument cannot take the output: a scalar argument bound to a mesh (`Source data item is not one-dimensional`), or to an output of another data kind (`Source data kind mismatch`). | Bind an output of the argument's kind; a mesh goes to a `PolyhedralSurfaceArgument`, a series to a `TimeSeriesArgumentDouble`. |
| `Component "<c>", argument "<a>": provider "<p>" failed validation:` and the problems | The bound provider is run before the consumer, and its `validate()` reported problems. | Fix the provider's arguments. |
| `Component "<c>", argument "<a>": provider "<p>" failed while producing its outputs`, and its errors | The bound provider failed during its run. | Read the provider's errors. |
| `components["<c>"].arguments.<a>.@from.component "<p>" is not declared in components[]` | The binding names a component the document does not declare. | Declare the provider in the same document. |

Values must be set before `initialize()`: components read their arguments there, and
`initialize()` again recreates the ports and drops their connections. The interval pair
refuses a new `config` outright once initialization has started.

## A run does not do what it should

**The workflow fails with ``component '<id>' failed during update``.** A component moved to
Failed in `update()`; its `errors()` hold the reason, for example the mesh generator's tool
message or the interval receiver's check. The time-stepped workflow stops at the first
failure.

**The run never reaches Done, or ends too early.** In a time-stepped workflow the run is
Done when every component is Done or Finished. The signal source and the interval source
decide when that is; the consumers follow their providers. A consumer with no provider is
Done after one update, so an unconnected recorder records one row. Check the source's
`steps` and the connections.

**A relative path lands somewhere unexpected.** The components resolve relative paths
differently:

| Component | Argument | Relative to |
| --- | --- | --- |
| Recorder | `path` | the reference directory when set, else the working directory |
| Interval receiver | `receipt_file` in `config` | the reference directory (the working directory when it is empty) |
| Time Series Provider | `source`, `cache` | the process's working directory |
| Mesh Generator | `output_path` | the process's working directory |

Composer sets the reference directory to the composition's directory; a test or a custom
host sets it with `setReferenceDirectory()` or leaves it empty. Give absolute paths to the
time series provider and the mesh generator when the host's working directory is not the
composition's.

**`errors()` holds `illegal status transition ...` after a successful run.** The SDK
records every status change the interfaces' transition table rejects, once per pair of
states, and still makes it. Two come from these plugins: the time series provider's
`Updated -> Done` at every run, and `Finished -> Validating` for a provider bound with
`@from` that a workflow validates again (see the @ref cookbook_bound "cookbook"). Neither
changes a result.

**A web source fails with ``No resolver is installed for 'https' URIs``.** The time series
provider fetches through the SDK's process-wide resolver, which Composer installs and a
custom host must install itself with `IO::setUriResolver()`. Without a network, set `cache`
and run once with it.

**A recording ends early.** The recorder's rows are buffered until `finish()` closes the
file. Read the file after the run's `finish()`.

**A host lists the Basic Adapters but cannot place them.** Adapter factories are not model
components; they attach to connections (@ref ref_basicadapters "Basic Adapters").
