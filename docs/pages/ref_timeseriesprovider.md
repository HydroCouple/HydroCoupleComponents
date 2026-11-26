# Time Series Provider {#ref_timeseriesprovider}

[TOC]

The time series provider reads one source, a file or a web service, and exposes one
time-series output per series it finds, ready for an exchange connection or an argument
binding. It leaves the numbers as the source holds them; the only conversion is from
calendar text to Julian days, the SDK's time convention, done once here instead of by every
consumer.

| | |
| --- | --- |
| Library | `hcc_timeseriesprovider`, built as `build/components/libhcc_timeseriesprovider.so` |
| Component info ID | `hydrocouple.components.timeseriesprovider` |
| Caption, version, tags | Time Series Provider; 0.1.0; Provider, Time Series |
| Default instance ID | `hydrocouple.components.timeseriesprovider.instance` |
| Interfaces | info: `IModelComponentInfo`; instance: `IModelComponent`; outputs: `Temporal::ITimeSeriesComponentDataItem` |
| Source | `components/timeseriesprovider/timeseriesprovidercomponent.cpp` |

## Arguments

| ID | Type | Default | Units | Valid range | What it does |
| --- | --- | --- | --- | --- | --- |
| `source` | String | empty | none | empty, a file path, or an `http` or `https` URL | What to read. Empty is a valid provider with no series. Surrounding whitespace is trimmed. File filters: `Time Series (*.csv *.npt *.dat)`, `CSV Files (*.csv)`, `CE-QUAL-W2 Series (*.npt)`, `SWMM Series (*.dat)`. |
| `format` | String | `auto` | none | `auto` (or empty), `csv`, `w2`, `swmm`, `nwis`, `netcdf` | The reader. `auto` chooses from the file extension or the URL (below). The argument's Quality offers the first five; `netcdf` is accepted and refused (see NetCDF). Case-sensitive. |
| `cache` | String, optional | empty | none | empty or a file path | For web sources only: the file a successful fetch is written to, and the file a failed fetch falls back on. Ignored for files. |

A relative `source` or `cache` path is opened relative to the process's working directory,
not the component's reference directory and not the composition's directory. A
composition that names `demo_met.csv` therefore finds it only when the host runs from the
directory holding it; `DemoTest` changes the working directory for this reason. Give an
absolute path when the host's working directory is not known.

### Choosing the reader

| `source` | `format` `auto` chooses | Reader |
| --- | --- | --- |
| file ending `.csv` | `csv` | delimited CSV |
| file ending `.npt` | `w2` | CE-QUAL-W2 input deck |
| file ending `.dat` | `swmm` | SWMM-style external time series |
| file ending `.nc` | `netcdf` | refused, see NetCDF |
| any other file | refused | |
| URL containing `usgs.gov` or `/nwis/` | `nwis` | USGS NWIS JSON |
| any other URL | refused | |

Extensions are matched with their case: `flows.CSV` needs `format` set to `csv`. An
explicit `format` overrides the extension for files; for URLs only `nwis` is accepted, and
for files `nwis` is unknown.

### Times

A time is a number, taken as a Julian day as it is, or calendar text in one of two forms,
converted with `TimeData::gregorianToJulianDay`:

| Form | Examples | Rule |
| --- | --- | --- |
| ISO | `2020-01-31`, `2020-01-31 06:30`, `2020-01-31T06:30:00` | the first `-` is the fifth character; any one character separates date and time |
| US | `01/31/2020`, `01/31/2020 06:30`, `1/31/2020 6:30:15` | month/day/year; the year is taken as written, so `01/31/20` is the year 20 |

The time of day is optional in both. Text after the seconds, such as fractions of a second
or a time zone offset, is ignored, and the fields are not range-checked: `2020-13-45` is
converted, not refused. NWIS stamps such as `2020-01-01T06:00:00.000-07:00` are therefore
read as 06:00 on the given day with the offset dropped, so series from two zones are not
silently shifted against each other.

## Formats

### CSV

The first column is time and every other column is one series. When the first line's time
field reads as neither a number nor a date, that line is a header naming the series; without
one they are named `<file stem>_<column>`, counting the value columns from 1. Only the first
line can be a header: a later line with an unreadable time is an error. Blank lines and
lines starting with `#` are skipped, fields are split at every comma (there is no
quoting), and every row must have as many values as the first data row. The fixture
`tests/fixtures/timeseries/flows.csv`:

\verbinclude flows.csv

gives two outputs, `flow` and `stage`, each with three times. A file without a header,
such as `plain.csv`:

\verbinclude plain.csv

gives one output, `plain_1`, at the Julian days 2458849.5 and 2458850.

### CE-QUAL-W2 (.npt)

A title line, a header line naming the columns, then whitespace-separated rows of Julian
day and values. Blank lines and lines starting with `$` or `#` are skipped. A row is data
when its first token is a number; the first other row is the title, and any later one
replaces the column names. The day column must be a number: calendar text is read as the
title or a header. The first data row fixes the number of series; extra values on later
rows are ignored and missing ones are skipped, so series can end up with different
lengths. `met.npt`:

\verbinclude met.npt

gives `TAIR` and `TDEW`, each at days 1.0, 1.5 and 2.0.

### SWMM-style (.dat)

`date time value` rows, where a `time value` row reuses the previous row's date, the way
SWMM's external time-series files carry a day of readings. Lines starting with `;` are
comments, and tokens after the third are ignored. One series, named after the file stem.
`rain.dat`:

\verbinclude rain.dat

gives one output, `rain`, with four times: January 31, 2020 at 00:00, 06:00 and 12:00, and
February 1 at 00:00.

### USGS NWIS (web)

The JSON answer of the NWIS instantaneous-values or daily-values service. Each entry of
`value.timeSeries` with readings becomes one series, named `<site>_<parameter>`:

| Part | Taken from | When it is missing |
| --- | --- | --- |
| `<site>` | `sourceInfo.siteCode[0].value` | `site` |
| `<parameter>` | `variable.variableCode[0].value` | `param` |

An entry with neither code is therefore named `site_param`. Only the first block of an entry's `values` array is read; an
entry without values, or with an empty first block, is skipped. Each reading's `value` is a
string read as a number and its `dateTime` is read as described under Times. The fixture
`nwis_iv.json` gives `09380000_00060` (discharge) and `09380000_00065` (gage height).

### NetCDF

A file ending in `.nc`, or `format` set to `netcdf`, is refused with directions:
NetCDF CF time series are not implemented yet. Convert the series to CSV. The refusal is
the reader's, not a missing NetCDF library: it happens with every SDK build.

## Web sources and the cache

The provider has no network code. It fetches through the SDK's process-wide URI resolver
(`IO::readReference`), which the host installs: Composer installs an HTTP resolver, and the
tests install a stub. Without one, every web source fails with
``No resolver is installed for 'https' URIs``.

| Fetch | `cache` set | Result |
| --- | --- | --- |
| succeeds | yes | the answer is written to `cache` (overwriting it), then read |
| succeeds | no | the answer is read |
| fails | yes, file exists | the cached answer is read: a rerun without a network serves the numbers the original run fetched |
| fails | yes, no file | initialization fails, naming the fetch failure and the cache path |
| fails | no | initialization fails, naming the fetch failure |

The cache is written only after a successful fetch, in an existing directory. To refresh
it, initialize again with the network back.

## Inputs and outputs

| Port | ID | Quantity and units | Geometry | Time support |
| --- | --- | --- | --- | --- |
| Output (one per series) | the series name | a unitless quantity named after the series; Float64 | none; one value per time step on the dimension `time` | every time step of the source, in Julian days, in the source's order |

Series names must be unique. The SDK's output registry keeps the first output with a given
ID and leaves out later ones, without a message, so two CSV columns with the same header,
or two NWIS entries without codes, yield one output. Each output is an SDK
`Temporal::TimeSeriesOutputDouble`. Its `times()` holds the Julian
days, `timeCount()` their number, and its values are read through `getValuesInto()` with the
item's `shape()`. Units are not read from any format, NWIS included: a consumer must know
them. A consumer adopts a series with an SDK `Temporal::TimeSeriesArgumentDouble`, through
an argument binding, or connects an input to it, usually through a temporal interpolation
adapter.

## Behavior

### initialize()

Reads the source, because the outputs are the source's series and a host initializes a
component to learn its outputs. On success it creates one output per series in the order
the source holds them; on failure the status is Failed with the reader's message and no
outputs exist. Initializing again rereads the source and recreates the outputs, which drops
their connections.

### validate()

Moves to Valid: the source was checked while the component initialized.

### prepare()

Moves to Updated with the message `prepared`.

### update()

Changes nothing and reports Done with the message `series served`. It moves from Updated to
Done without passing through Updating, a transition the interface's status table rejects,
so the SDK queues `illegal status transition Updated -> Done
(Helpers::isValidComponentStatusTransition rejects it)` on `errors()` at the first update.
The run is not affected; a host that treats every queued error as a failure will report it.

### finish()

Moves through Finishing to Finished. The outputs keep their series.

## Status messages

A run over `flows.csv`, and what `errors()` holds after it:

```text
[timeseriesprovider] status messages of a run
    Initializing "Initializing component"
    Initialized "Component initialized successfully"
    Validating ""
    Valid ""
    Preparing ""
    Updated "prepared"
    Done "series served"
    Finishing ""
    Finished ""
  errors() after the run
    illegal status transition Updated -> Done (Helpers::isValidComponentStatusTransition rejects it)
```

## Errors

Every failure happens in `initialize()`, so the status is Failed and the message is on
`errors()` with the SDK's prefix `Argument initialization failed: `. Produced by
`docs/examples/reference_messages` from the inputs in its `inputs/` directory:

```text
[timeseriesprovider] errors
  source mystery.xyz
    Argument initialization failed: Cannot tell the format of "mystery.xyz" from its extension; tried .csv, .npt, .dat, .nc — set the format argument explicitly
  source levels.nc
    Argument initialization failed: NetCDF CF time series are not implemented yet; convert to CSV, or wait for the registry's next entry
  source missing.csv
    Argument initialization failed: Cannot open "missing.csv"
  format "xml"
    Argument initialization failed: Unknown format "xml"; the registry knows csv, w2, swmm
  format "nwis" for a file
    Argument initialization failed: Unknown format "nwis"; the registry knows csv, w2, swmm
  CSV row with one field
    Argument initialization failed: CSV line 3 has fewer than two columns
  CSV time that is not a time
    Argument initialization failed: CSV line 2: cannot read "yesterday" as a time
  CSV row with too few values
    Argument initialization failed: CSV line 3 has 1 values but 2 series
  CSV value that is not a number
    Argument initialization failed: CSV line 2: cannot read "lots" as a number
  CSV with a header and no rows
    Argument initialization failed: The CSV holds no data rows
  .npt value that is not a number
    Argument initialization failed: W2 row "1.0 warm": cannot read "warm" as a number
  .npt with no rows
    Argument initialization failed: The .npt deck holds no data rows
  .dat line with one field
    Argument initialization failed: Cannot read .dat line "0.5": expected date time value
  .dat date that is not a date
    Argument initialization failed: Cannot read "tomorrow 00:00" as a date and time
  .dat value that is not a number
    Argument initialization failed: Cannot read "lots" as a number
  .dat with no rows
    Argument initialization failed: The .dat file holds no data rows
  web source with no resolver installed
    Argument initialization failed: The fetch failed (No resolver is installed for 'https' URIs, so this argument cannot read https://waterservices.usgs.gov/nwis/iv/?sites=09380000&format=json) and no cache argument is set to fall back on
  web source not recognized as NWIS
    Argument initialization failed: Unrecognized web service "https://example.org/series.json"; the registry knows USGS NWIS — set the format argument to "nwis" if this is one
  web source with format "csv"
    Argument initialization failed: Web sources support only the "nwis" format today
  answer that is not JSON
    Argument initialization failed: The service's answer is not JSON: [json.exception.parse_error.101] parse error at line 1, column 2: syntax error while parsing value - invalid literal; last read: 'th'
  answer without value.timeSeries
    Argument initialization failed: The answer carries no value.timeSeries block; is this the NWIS JSON service?
  answer with no series
    Argument initialization failed: The service answered, but with no series — check the site and parameter codes
  answer with an unreadable dateTime
    Argument initialization failed: Cannot read "noon" as a date and time
  answer with an unreadable value
    Argument initialization failed: Cannot read "Ice" as a number
  cache below a regular file
    Argument initialization failed: Fetched, but cannot write the cache "recorder_run.csv/nwis.json"
  offline, no cache argument
    Argument initialization failed: The fetch failed (the stub is offline) and no cache argument is set to fall back on
  offline, cache file missing
    Argument initialization failed: The fetch failed (the stub is offline) and there is no cache at "no-such-cache.json"
  series with no siteCode and no variableCode is named
    site_param
```

| Message (after the prefix) | Cause | Fix |
| --- | --- | --- |
| `Cannot tell the format of "<path>" from its extension; tried .csv, .npt, .dat, .nc — set the format argument explicitly` | The file's extension is none of the four, or differs in case. | Set `format` to `csv`, `w2` or `swmm`. |
| ``NetCDF CF time series are not implemented yet; convert to CSV, or wait for the registry's next entry`` | A `.nc` file, or `format` `netcdf`. | Convert the series to CSV. |
| `Cannot open "<path>"` | The file does not exist, or a relative path is resolved against another working directory. | Check the path against the host's working directory, or give an absolute path. |
| `Unknown format "<f>"; the registry knows csv, w2, swmm` | `format` is not one of the readers, or is `nwis` for a file. | Use `auto`, `csv`, `w2` or `swmm` for files. |
| `CSV line <n> has fewer than two columns` | A row, or the header, has no comma. | Give every line a time and at least one value. |
| `CSV line <n>: cannot read "<t>" as a time` | A time field after the first data line is neither a number nor ISO or US calendar text. | Fix the time, or move the header to the first line. |
| `CSV line <n> has <k> values but <m> series` | A row has a different number of values from the first data row. | Fill every column of every row. |
| `CSV line <n>: cannot read "<v>" as a number` | A value field is empty or not a number. | Write a number; the reader has no missing-value marker. |
| `The CSV holds no data rows` | Only a header, comments or blank lines. | Add rows. |
| `W2 row "<row>": cannot read "<v>" as a number` | A value in a W2 data row is not a number. | Fix the value. |
| `The .npt deck holds no data rows` | No row starts with a number. | Add rows. |
| `Cannot read .dat line "<line>": expected date time value` | A row with one token, or a two-token row before any dated row. | Give the first row a date, time and value. |
| `Cannot read "<date> <time>" as a date and time` | The date and time are not ISO or US calendar text. | Fix the date. |
| `Cannot read "<v>" as a number` | A `.dat` value or an NWIS reading is not a number. | Fix the value; NWIS marks such as `Ice` are not numbers. |
| `The .dat file holds no data rows` | Only comments or blank lines. | Add rows. |
| ``The fetch failed (No resolver is installed for 'https' URIs, ...)`` | No URI resolver is installed in the process: a headless host, a test or a script that did not install one. | Install a resolver with `IO::setUriResolver()` before initializing, or provide a cache. |
| `Unrecognized web service "<url>"; ...` | The URL is not recognized as NWIS. | Set `format` to `nwis` if it is an NWIS URL. |
| `Web sources support only the "nwis" format today` | A URL with `format` other than `auto` or `nwis`. | Download the file and use it as a file source. |
| ``The service's answer is not JSON: <parser message>`` | The service answered with an error page or another format. | Ask for `format=json` in the URL; check the URL in a browser. |
| `The answer carries no value.timeSeries block; is this the NWIS JSON service?` | The JSON is not an NWIS time-series answer. | Use the NWIS `iv` or `dv` service. |
| `The service answered, but with no series — check the site and parameter codes` | No entry had readings: wrong site or parameter codes, or a period with no data. | Check the codes and the period in the URL. |
| `Cannot read "<stamp>" as a date and time` | An NWIS `dateTime` is not ISO text. | Report it; the service writes ISO stamps. |
| `Fetched, but cannot write the cache "<path>"` | The cache's directory does not exist or is a file. | Create the directory, or choose another path. |
| `The fetch failed (<reason>) and there is no cache at "<path>"` | Offline, and no earlier run wrote the cache. | Run once with the network to fill the cache. |
| `The fetch failed (<reason>) and no cache argument is set to fall back on` | Offline, with no `cache`. | Set `cache` and run once with the network. |

## Example composition

`compositions/demo_a.json` reads `demo_met.csv`, a one-series CSV:

\verbinclude demo_met.csv

\dontinclude demo_a.json
\skip "id": "met"
\until "source"

and feeds its `air_temperature` series to FVQual twice: through a `@from` binding to the
solver's `met_air_temperature` argument, and through a connection carrying the SDK's
`temporal_interpolation` adapter to its `air_temperature` input. The @ref cookbook "cookbook"
describes the run.

A consumer's `TimeSeriesArgumentDouble` bound to the `TAIR` column of `met.npt`, as
`TimeSeriesProviderTest.ASeriesArgumentBindsToAProvidersOutput` applies it:

\dontinclude test_timeseriesprovider.cpp
\skip const nlohmann::json document = {
\until fixture("met.npt")

## Tests

| Test | What it checks |
| --- | --- |
| `TimeSeriesProviderTest.CsvColumnsBecomeOutputsWithParsedDatetimes` | `flows.csv` gives `flow` and `stage` with ISO times converted to Julian days |
| `TimeSeriesProviderTest.ANumericFirstColumnIsTakenAsTimeDirectly` | `plain.csv`'s numeric times are taken as Julian days |
| `TimeSeriesProviderTest.W2NptColumnsCarryTheirHeaderNames` | `met.npt` gives `TAIR` and `TDEW` |
| `TimeSeriesProviderTest.ASwmmDatCarriesTheDateForward` | `rain.dat`'s two-token rows reuse the date |
| `TimeSeriesProviderTest.AnUnknownExtensionFailsNamingWhatWasTried` | the message names `.csv` and the `format` argument |
| `TimeSeriesProviderTest.NetcdfIsRefusedWithDirectionsNotMisread` | a `.nc` source is refused naming NetCDF |
| `TimeSeriesProviderTest.ASeriesArgumentBindsToAProvidersOutput` | a `@from` binding hands `TAIR` to a consumer's series argument |
| `TimeSeriesProviderTest.NwisSeriesAreNamedSiteAndParameter` | NWIS series names, values, and times six hours apart |
| `TimeSeriesProviderTest.AFetchIsCachedAndServedOffline` | a fetch fills the cache; offline, the cache serves the same values without a second request |
| `TimeSeriesProviderTest.AnEmptyAnswerFailsPointingAtTheCodes` | an empty NWIS answer points at the site and parameter codes |
| `TimeSeriesProviderTest.OfflineWithNoCacheFailsNamingBoth` | offline without a cache names both |
| `DemoTest.*` | `demo_met.csv` drives FVQual; every recorded air temperature is 5 |
| `docs.example.reference_messages` | the status messages, the errors above and the `site_param` name |
