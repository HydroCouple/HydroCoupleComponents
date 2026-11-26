// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The component reference pages' status and error tables, produced by the
// plugins themselves. The program loads every shipped plugin the way a host
// does, records the status messages of one normal run of each component,
// then provokes each error the pages list and prints the message a host
// sees. It fails when a listed message no longer appears, so the tables
// cannot drift from the code.

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/core/identity.h"
#include "hydrocouplesdk/io/modelinitializer.h"
#include "hydrocouplesdk/io/uriresolver.h"

#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Status = IModelComponent::ComponentStatus;
using StatusEvent = std::shared_ptr<IComponentStatusChangeEventArgs>;

namespace
{

int g_failures = 0;

//! Every shipped plugin, loaded once and closed after every instance died.
class Plugins
{
  public:
    Plugins()
    {
        for (const char *stem :
             {"hcc_smoke", "hcc_signalsource", "hcc_gaintransformer",
              "hcc_recorder", "hcc_combiner", "hcc_basicadapters",
              "hcc_meshgenerator", "hcc_timeseriesprovider",
              "hcc_intervalsource", "hcc_intervalreceiver"})
        {
            fs::path path = fs::path(HCC_COMPONENTS_DIR) /
                            (std::string("lib") + stem + ".so");
            if (!fs::exists(path))
                path.replace_extension(".dylib");
            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            auto abi = handle ? reinterpret_cast<HydroCoupleComponentAbiFn>(
                                    dlsym(handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL))
                              : nullptr;
            auto info = handle ? reinterpret_cast<HydroCoupleComponentInfoFn>(
                                     dlsym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL))
                               : nullptr;
            if (!abi || std::string(abi()) != HYDROCOUPLE_COMPONENT_ABI_STAMP ||
                !info)
            {
                std::cout << "cannot load " << path << '\n';
                ++g_failures;
                continue;
            }
            m_handles.push_back(handle);
            m_infos[info()->id()] = info();
        }
    }

    ~Plugins()
    {
        for (void *handle : m_handles)
            dlclose(handle);
    }

    std::unique_ptr<IModelComponent> create(const std::string &id) const
    {
        auto *info = dynamic_cast<IModelComponentInfo *>(m_infos.at(id));
        return info->createComponentInstance();
    }

    std::unique_ptr<IAdaptedOutputFactoryComponent> factory() const
    {
        auto *info = dynamic_cast<IAdaptedOutputFactoryComponentInfo *>(
            m_infos.at("hydrocouple.components.adapters.basic"));
        return info->createComponentInstance();
    }

  private:
    std::vector<void *> m_handles;
    std::map<std::string, IComponentInfo *> m_infos;
};

//! Records each status change as `Status "message"`.
class StatusLog final : public ISlot<const StatusEvent &>
{
  public:
    void operator()(const ISignal<const StatusEvent &> &,
                    const StatusEvent &event) override
    {
        entries.push_back(
            AbstractModelComponent::statusToString(event->status()) + " \"" +
            event->message() + "\"");
    }
    std::vector<std::string> entries;
};

std::shared_ptr<StatusLog> listen(IModelComponent &component)
{
    auto log = std::make_shared<StatusLog>();
    component.connect(log);
    return log;
}

void printLog(const StatusLog &log)
{
    for (const std::string &entry : log.entries)
        std::cout << "    " << entry << '\n';
}

//! Sets one argument from a JSON payload; returns the argument's message.
std::string set(IModelComponent &component, const std::string &id,
                const Json &payload)
{
    for (IArgument *argument : component.arguments())
    {
        if (argument->id() != id)
            continue;
        std::string message;
        if (!argument->initialize(payload.dump(),
                                  IArgument::ArgumentInputType::JSON, message))
            return message;
        return {};
    }
    return "no argument " + id;
}

Json values(const Json &value) { return {{"values", Json::array({value})}}; }

//! The messages a host reads from errors() after a failure, one per line.
std::string errorsOf(IModelComponent &component)
{
    std::string text;
    for (const ErrorEntry &entry : component.errors(true))
        text += (text.empty() ? "" : "\n") + entry.message;
    return text;
}

//! Prints one case and checks that the message holds the expected text.
void report(const std::string &label, const std::string &message,
            const std::string &expected)
{
    std::cout << "  " << label << '\n';
    std::istringstream lines(message);
    for (std::string line; std::getline(lines, line);)
        std::cout << "    " << line << '\n';
    if (message.find(expected) == std::string::npos)
    {
        std::cout << "    ^ MISSING: " << expected << '\n';
        ++g_failures;
    }
}

//! Initializes a fresh instance with the given arguments; returns errors().
std::string initializeWith(const Plugins &plugins, const std::string &id,
                           const std::map<std::string, Json> &arguments)
{
    std::unique_ptr<IModelComponent> component = plugins.create(id);
    for (const auto &[argument, payload] : arguments)
    {
        const std::string message = set(*component, argument, payload);
        if (!message.empty())
            return "argument " + argument + ": " + message;
    }
    try
    {
        component->initialize();
    }
    catch (const std::exception &error)
    {
        return std::string("initialize() threw: ") + error.what();
    }
    return errorsOf(*component);
}

//! Runs an initialized component to Finished and returns its errors().
std::string run(IModelComponent &component)
{
    const std::vector<std::string> problems = component.validate();
    if (!problems.empty())
        return problems.front();
    component.prepare();
    for (int step = 0; step < 100 && component.status() != Status::Done &&
                       component.status() != Status::Failed;
         ++step)
        component.update();
    if (component.status() == Status::Done)
        component.finish();
    return errorsOf(component);
}

std::string input(const std::string &name)
{
    return (fs::path(DOCS_EXAMPLE_SOURCE_DIR) / "inputs" / name).string();
}

std::string fixture(const std::string &name)
{
    return (fs::path(DOCS_EXAMPLE_SOURCE_DIR) / ".." / ".." / ".." / "tests" /
            "fixtures" / "timeseries" / name)
        .lexically_normal()
        .string();
}

//! Applies a composition to fresh instances; returns the initializer's
//! message.
std::string applyDocument(const Plugins &plugins, const Json &document)
{
    std::vector<std::unique_ptr<IModelComponent>> instances;
    std::map<std::string, IModelComponent *> byId;
    for (const Json &block : document["components"])
    {
        instances.push_back(plugins.create(
            block["info"]["component_info_id"].get<std::string>()));
        byId[block["id"].get<std::string>()] = instances.back().get();
    }
    std::unique_ptr<IAdaptedOutputFactoryComponent> factory =
        plugins.factory();
    IO::ModelInitializer initializer(
        [&byId](const std::string &id) -> IModelComponent * {
            const auto found = byId.find(id);
            return found == byId.end() ? nullptr : found->second;
        });
    initializer.setAdapterFactoryResolver([&factory] {
        return std::vector<IAdaptedOutputFactory *>{factory.get()};
    });
    std::string message;
    if (initializer.initializeFromJsonText(document.dump(), message))
        return "applied";
    return message;
}

Json block(const std::string &id, const std::string &info,
           const Json &arguments = Json::object())
{
    return {{"id", id},
            {"info", {{"component_info_id", info}}},
            {"arguments", arguments}};
}

//! A process-wide resolver for the web-source cases, as a host installs.
class StubResolver final : public IO::UriResolver
{
  public:
    bool handles(const std::string &scheme) const override
    {
        return scheme == "http" || scheme == "https";
    }
    bool resolve(const std::string &, std::string &contents,
                 std::string &message) override
    {
        if (offline)
        {
            message = "the stub is offline";
            return false;
        }
        contents = payload;
        return true;
    }
    std::string payload;
    bool offline = false;
};

std::string fileText(const std::string &path)
{
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

const std::string kSignal = "hydrocouple.components.signal_source";
const std::string kGain = "hydrocouple.components.gain";
const std::string kRecorder = "hydrocouple.components.recorder";
const std::string kCombiner = "hydrocouple.components.combiner";
const std::string kMesh = "hydrocouple.components.meshgenerator";
const std::string kSeries = "hydrocouple.components.timeseriesprovider";
const std::string kSource = "org.hydrocouple.openswmm.test.interval-source";
const std::string kReceiver = "org.hydrocouple.openswmm.test.sink";

// ── One section per reference page ────────────────────────────────────────

void smoke(const Plugins &plugins)
{
    std::cout << "\n[smoke] status messages of one run\n";
    std::unique_ptr<IModelComponent> component =
        plugins.create("hydrocouple.components.smoke");
    const auto log = listen(*component);
    component->initialize();
    run(*component);
    printLog(*log);
}

void signalSource(const Plugins &plugins)
{
    std::cout << "\n[signal_source] status messages of a two-step run\n";
    std::unique_ptr<IModelComponent> component = plugins.create(kSignal);
    set(*component, "steps", values(2));
    const auto log = listen(*component);
    component->initialize();
    run(*component);
    printLog(*log);

    std::cout << "[signal_source] errors\n";
    report("waveform \"triangle\"",
           initializeWith(plugins, kSignal, {{"waveform", values("triangle")}}),
           "is not one of sine, square, ramp, constant");
    report("steps 0", initializeWith(plugins, kSignal, {{"steps", values(0)}}),
           "steps must be at least 1, not 0");
    report("amplitude \"two\"",
           initializeWith(plugins, kSignal, {{"amplitude", values("two")}}),
           "type must be number");
    report("amplitude with no values",
           initializeWith(plugins, kSignal,
                          {{"amplitude", Json{{"values", Json::array()}}}}),
           "initialize() threw");
    report("amplitude with no values, through a composition",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("source", kSignal,
                                      {{"amplitude",
                                        {{"values", Json::array()}}}})})}}),
           "JSON parse error");
}

void gain(const Plugins &plugins)
{
    std::cout << "\n[gain] status messages of a run behind a two-step "
                 "source\n";
    std::unique_ptr<IModelComponent> source = plugins.create(kSignal);
    std::unique_ptr<IModelComponent> component = plugins.create(kGain);
    set(*source, "steps", values(2));
    source->initialize();
    component->initialize();
    source->outputs()[0]->addConsumer(component->inputs()[0]);
    const auto log = listen(*component);
    (void)source->validate();
    (void)component->validate();
    source->prepare();
    component->prepare();
    while (component->status() == Status::Updated)
    {
        source->update();
        component->update();
    }
    component->finish();
    printLog(*log);
}

void recorder(const Plugins &plugins)
{
    std::cout << "\n[recorder] status messages of a one-step run\n";
    std::unique_ptr<IModelComponent> component = plugins.create(kRecorder);
    set(*component, "path", values("recorder_run.csv"));
    const auto log = listen(*component);
    component->initialize();
    run(*component);
    printLog(*log);

    std::cout << "[recorder] errors\n";
    report("path empty (the default)", initializeWith(plugins, kRecorder, {}),
           "path is required");

    std::unique_ptr<IModelComponent> unwritable = plugins.create(kRecorder);
    set(*unwritable, "path", values("recorder_run.csv/recorded.csv"));
    unwritable->initialize();
    report("path below a regular file", run(*unwritable), "for writing");
}

void combiner(const Plugins &plugins)
{
    std::cout << "\n[combiner] status messages of a one-step run\n";
    std::unique_ptr<IModelComponent> component = plugins.create(kCombiner);
    const auto log = listen(*component);
    component->initialize();
    run(*component);
    printLog(*log);

    std::cout << "[combiner] errors\n";
    report("operation \"max\"",
           initializeWith(plugins, kCombiner, {{"operation", values("max")}}),
           "is not one of sum, product, mean");

    std::unique_ptr<IModelComponent> sums = plugins.create(kCombiner);
    std::unique_ptr<IModelComponent> source = plugins.create(kSignal);
    std::unique_ptr<IModelComponent> strings = plugins.create(kSource);
    sums->initialize();
    source->initialize();
    set(*strings, "config",
        Json{{"schema", "hydrocouple.interval-transfer/1"},
             {"epoch_julian_day", 2440587.5},
             {"species", Json::array()},
             {"intervals",
              Json::array({{{"start_s", 0}, {"end_s", 60}, {"water_m3", 1},
                            {"enthalpy_j", 0}, {"species", Json::object()}}})}});
    strings->initialize();
    auto *operands = dynamic_cast<IMultiInput *>(sums->inputs()[0]);
    IOutput *signal = source->outputs()[0];
    std::string message;
    (void)sums->inputs()[0]->canConsume(signal, message);
    report("canConsume without a role argument", message, "takes providers by role");
    (void)operands->canConsume(signal, message, nullptr);
    report("canConsume with no role", message, "name a provider role");
    Identity stranger("multiplicand", "Multiplicand");
    (void)operands->canConsume(signal, message, &stranger);
    report("canConsume with role \"multiplicand\"", message,
           "is not a provider role here");
    Identity augend("augend", "Augend");
    (void)operands->canConsume(strings->outputs()[0], message, &augend);
    report("canConsume of a String output", message, "rank-1 Double output");
    const int64_t one = 1, zero = 0;
    double value = 1;
    BufferDescriptor buffer{&value, DataKind::Float64, 1, &one, nullptr, 0};
    (void)operands->setValuesFrom(buffer, {&zero, 1}, {&one, 1}, &message);
    report("setValuesFrom", message, "pulled from providers");

    const Json roleless = {
        {"components", Json::array({block("first", kSignal),
                                    block("combiner", kCombiner)})},
        {"connections",
         Json::array({{{"from", {{"component", "first"}, {"output", "signal"}}},
                       {"to", {{"component", "combiner"}, {"input", "operands"}}}}})}};
    report("composition connection without to.role", applyDocument(plugins, roleless),
           "name a provider role");
    Json unknownRole = roleless;
    unknownRole["connections"][0]["to"]["role"] = "multiplicand";
    report("composition connection with to.role \"multiplicand\"",
           applyDocument(plugins, unknownRole), "has no provider role");
}

void basicAdapters(const Plugins &plugins)
{
    std::cout << "\n[basic_adapters] errors\n";
    std::unique_ptr<IAdaptedOutputFactoryComponent> factory = plugins.factory();
    std::unique_ptr<IModelComponent> source = plugins.create(kSignal);
    source->initialize();
    IOutput *signal = source->outputs()[0];
    std::unique_ptr<IAdaptedOutput> negate = factory->createAdaptedOutput(
        factory->getAvailableAdaptedOutputIds(signal)[0], signal);
    double value = 0;
    const int64_t one = 1, zero = 0;
    BufferDescriptor buffer{&value, DataKind::Float64, 1, &one, nullptr, 0};
    std::string message;
    (void)negate->getValuesInto(buffer, {&zero, 1}, {&one, 1}, &message);
    report("reading before initialize()", message, "initialize() has not run");
    (void)negate->setValuesFrom(buffer, {&zero, 1}, {&one, 1}, &message);
    report("setValuesFrom", message, "read-only");
    (void)signal->removeAdaptedOutput(negate.get());

    const Json meshToGain = {
        {"components",
         Json::array({block("meshgen", kMesh), block("gain", kGain)})},
        {"connections",
         Json::array({{{"from",
                        {{"component", "meshgen"},
                         {"output", "mesh"},
                         {"adapted_outputs",
                          Json::array({{{"factory",
                                         "hydrocouple.components.adapters.basic"},
                                        {"id", "negate"}}})}}},
                       {"to", {{"component", "gain"}, {"input", "in"}}}}})}};
    report("negate requested for the mesh output", applyDocument(plugins, meshToGain),
           "no adapted output \"negate\" is offered");
}

void meshGenerator(const Plugins &plugins)
{
    std::cout << "\n[meshgenerator] status messages of a run with "
                 "quad_quality 0.6\n";
    std::unique_ptr<IModelComponent> component = plugins.create(kMesh);
    set(*component, "max_edge_length", values(50.0));
    set(*component, "quad_quality", values(0.6));
    const auto log = listen(*component);
    component->initialize();
    run(*component);
    printLog(*log);

    std::cout << "[meshgenerator] errors\n";
    std::unique_ptr<IModelComponent> invalid = plugins.create(kMesh);
    set(*invalid, "boundary", Json{{"values", {{0, 0, 0}, {10, 0, 0}}}});
    set(*invalid, "quad_quality", values(1.5));
    invalid->initialize();
    std::string problems;
    for (const std::string &problem : invalid->validate())
        problems += (problems.empty() ? "" : "\n") + problem;
    report("validate() of a 2 x 3 boundary and quad_quality 1.5", problems,
           "at least three vertices");
    report("errors() after that validate()", errorsOf(*invalid), "");

    std::unique_ptr<IModelComponent> collinear = plugins.create(kMesh);
    set(*collinear, "boundary", Json{{"values", {{0, 0}, {10, 0}, {20, 0}}}});
    collinear->initialize();
    report("a collinear boundary", run(*collinear), "triangulation");

    std::unique_ptr<IModelComponent> unwritable = plugins.create(kMesh);
    set(*unwritable, "output_path", values("recorder_run.csv/mesh.nc"));
    unwritable->initialize();
#ifdef USE_NETCDF
    report("output_path below a regular file", run(*unwritable),
           "Writing 'recorder_run.csv/mesh.nc' failed");
#else
    report("output_path with an SDK built without NetCDF", run(*unwritable),
           "this SDK build has no NetCDF support");
#endif

    std::unique_ptr<IModelComponent> meshed = plugins.create(kMesh);
    meshed->initialize();
    run(*meshed);
    double value = 0;
    const int64_t one = 1, zero = 0;
    BufferDescriptor buffer{&value, DataKind::Float64, 1, &one, nullptr, 0};
    std::string message;
    (void)meshed->outputs()[0]->getValuesInto(buffer, {&zero, 1}, {&one, 1},
                                              &message);
    report("reading values from the mesh output", message, "is geometry");
}

void timeSeries(const Plugins &plugins)
{
    std::cout << "\n[timeseriesprovider] status messages of a run\n";
    std::unique_ptr<IModelComponent> component = plugins.create(kSeries);
    set(*component, "source", values(fixture("flows.csv")));
    const auto log = listen(*component);
    component->initialize();
    const std::string afterRun = run(*component);
    printLog(*log);
    std::cout << "  errors() after the run\n    " << afterRun << '\n';

    std::cout << "[timeseriesprovider] errors\n";
    auto source = [&](const std::string &path,
                      const std::map<std::string, Json> &more = {}) {
        std::map<std::string, Json> arguments = more;
        arguments["source"] = values(path);
        return initializeWith(plugins, kSeries, arguments);
    };
    report("source mystery.xyz", source("mystery.xyz"),
           "Cannot tell the format");
    report("source levels.nc", source("levels.nc"),
           "NetCDF CF time series are not implemented yet");
    report("source missing.csv", source("missing.csv"), "Cannot open \"missing.csv\"");
    report("format \"xml\"", source(fixture("flows.csv"), {{"format", values("xml")}}),
           "Unknown format \"xml\"");
    report("format \"nwis\" for a file",
           source(fixture("flows.csv"), {{"format", values("nwis")}}),
           "Unknown format \"nwis\"");
    report("CSV row with one field", source(input("short_row.csv")),
           "has fewer than two columns");
    report("CSV time that is not a time", source(input("bad_time.csv")),
           "as a time");
    report("CSV row with too few values", source(input("ragged.csv")),
           "has 1 values but 2 series");
    report("CSV value that is not a number", source(input("bad_number.csv")),
           "as a number");
    report("CSV with a header and no rows", source(input("header_only.csv")),
           "The CSV holds no data rows");
    report(".npt value that is not a number", source(input("bad_number.npt")),
           "W2 row");
    report(".npt with no rows", source(input("header_only.npt")),
           "The .npt deck holds no data rows");
    report(".dat line with one field", source(input("bad_line.dat")),
           "expected date time value");
    report(".dat date that is not a date", source(input("bad_date.dat")),
           "as a date and time");
    report(".dat value that is not a number", source(input("bad_value.dat")),
           "as a number");
    report(".dat with no rows", source(input("header_only.dat")),
           "The .dat file holds no data rows");

    const std::string nwisUrl =
        "https://waterservices.usgs.gov/nwis/iv/?sites=09380000&format=json";
    report("web source with no resolver installed", source(nwisUrl),
           "No resolver is installed");

    StubResolver resolver;
    IO::setUriResolver(&resolver);
    report("web source not recognized as NWIS",
           source("https://example.org/series.json"),
           "Unrecognized web service");
    report("web source with format \"csv\"",
           source(nwisUrl, {{"format", values("csv")}}),
           "support only the \"nwis\" format");
    resolver.payload = fileText(input("nwis_not_json.txt"));
    report("answer that is not JSON", source(nwisUrl), "is not JSON");
    resolver.payload = fileText(input("nwis_no_timeseries.json"));
    report("answer without value.timeSeries", source(nwisUrl),
           "no value.timeSeries block");
    resolver.payload = fileText(fixture("nwis_empty.json"));
    report("answer with no series", source(nwisUrl), "check the site and parameter codes");
    resolver.payload = fileText(input("nwis_bad_time.json"));
    report("answer with an unreadable dateTime", source(nwisUrl),
           "Cannot read \"noon\" as a date and time");
    resolver.payload = fileText(input("nwis_bad_value.json"));
    report("answer with an unreadable value", source(nwisUrl),
           "Cannot read \"Ice\" as a number");
    resolver.payload = fileText(fixture("nwis_iv.json"));
    report("cache below a regular file",
           source(nwisUrl, {{"cache", values("recorder_run.csv/nwis.json")}}),
           "Fetched, but cannot write the cache");
    resolver.offline = true;
    report("offline, no cache argument", source(nwisUrl),
           "no cache argument is set to fall back on");
    report("offline, cache file missing",
           source(nwisUrl, {{"cache", values("no-such-cache.json")}}),
           "there is no cache at");

    resolver.offline = false;
    resolver.payload = fileText(input("nwis_unnamed.json"));
    std::unique_ptr<IModelComponent> unnamed = plugins.create(kSeries);
    set(*unnamed, "source", values(nwisUrl));
    unnamed->initialize();
    std::string names;
    for (IOutput *output : unnamed->outputs())
        names += output->id();
    report("series with no siteCode and no variableCode is named", names,
           "site_param");
    IO::setUriResolver(nullptr);
}

Json intervalConfiguration()
{
    return {{"schema", "hydrocouple.interval-transfer/1"},
            {"epoch_julian_day", 2440587.5},
            {"species", Json::array({{{"id", "NH4_N"}, {"basis", "kg-N"}}})}};
}

Json sourceConfiguration()
{
    Json config = intervalConfiguration();
    config["intervals"] = Json::array(
        {{{"start_s", 0}, {"end_s", 60}, {"water_m3", 2}, {"enthalpy_j", 100},
          {"species", {{"NH4_N", 0.2}}}}});
    return config;
}

Json receiverConfiguration()
{
    Json config = intervalConfiguration();
    config["expected_transfers"] = 1;
    config["receipt_file"] = "receipts.csv";
    return config;
}

void interval(const Plugins &plugins)
{
    std::cout << "\n[interval] status messages of a one-interval run\n";
    std::unique_ptr<IModelComponent> donor = plugins.create(kSource);
    std::unique_ptr<IModelComponent> sink = plugins.create(kReceiver);
    set(*donor, "config", sourceConfiguration());
    set(*sink, "config", receiverConfiguration());
    const auto donorLog = listen(*donor);
    const auto sinkLog = listen(*sink);
    donor->initialize();
    sink->initialize();
    for (IInput *field : sink->inputs())
        for (IOutput *offer : donor->outputs())
            if (offer->id() == field->id())
                offer->addConsumer(field);
    (void)donor->validate();
    (void)sink->validate();
    donor->prepare();
    sink->prepare();
    donor->update();
    sink->update();
    donor->finish();
    sink->finish();
    std::cout << "  source\n";
    printLog(*donorLog);
    std::cout << "  receiver\n";
    printLog(*sinkLog);

    std::cout << "[interval] errors\n";
    auto source = [&](const Json &config) {
        return initializeWith(plugins, kSource, {{"config", config}});
    };
    auto receiver = [&](const Json &config) {
        return initializeWith(plugins, kReceiver, {{"config", config}});
    };
    report("no config", initializeWith(plugins, kReceiver, {}),
           "attempting to parse an empty input");
    Json config = sourceConfiguration();
    config["schema"] = "hydrocouple.interval-transfer/2";
    report("schema hydrocouple.interval-transfer/2", source(config),
           "Use schema hydrocouple.interval-transfer/1");
    config = sourceConfiguration();
    config.erase("epoch_julian_day");
    report("no epoch_julian_day", source(config), "epoch_julian_day");
    config = sourceConfiguration();
    config["species"][0]["basis"] = "g";
    report("species basis \"g\"", source(config),
           "Use unique species IDs and a declared kg or kg-N basis");
    report("source scale 2",
           initializeWith(plugins, kSource,
                          {{"config", sourceConfiguration()},
                           {"scale", values(2.0)}}),
           "The controllable source requires scale 1");
    report("receiver scale -1",
           initializeWith(plugins, kReceiver,
                          {{"config", receiverConfiguration()},
                           {"scale", values(-1.0)}}),
           "scale must be one finite nonnegative number");
    config = sourceConfiguration();
    config["intervals"][0]["start_s"] = 10;
    report("first interval starting at 10 s", source(config),
           "Source intervals must be contiguous and increasing from zero");
    config = sourceConfiguration();
    config["intervals"][0]["species"] = Json::object();
    report("interval without its species amount", source(config),
           "Supply each configured species exactly once per interval");
    config = sourceConfiguration();
    config["intervals"][0]["water_m3"] = -1;
    report("negative water", source(config),
           "Supply finite values and nonnegative water/species amounts");
    config = sourceConfiguration();
    config["intervals"][0]["water_m3"] = 0;
    report("dry interval carrying heat", source(config),
           "A dry interval cannot transfer waterborne species or heat");
    config = sourceConfiguration();
    config["intervals"] = Json::array();
    report("no intervals", source(config), "Supply at least one source interval");
    config = receiverConfiguration();
    config["expected_transfers"] = 0;
    report("expected_transfers 0", receiver(config),
           "expected_transfers must be a positive integer");
    config = receiverConfiguration();
    config["receipt_file"] = "";
    report("empty receipt_file", receiver(config),
           "Supply a receipt_file and finite nonnegative max_water_m3");

    std::unique_ptr<IModelComponent> configured = plugins.create(kSource);
    set(*configured, "config", sourceConfiguration());
    configured->initialize();
    report("config set after initialize()",
           set(*configured, "config", sourceConfiguration()),
           "Configure before initialization");
    (void)configured->errors(true);
    configured->update();
    report("update() before prepare()", errorsOf(*configured),
           "Source is not ready to update");

    std::unique_ptr<IModelComponent> alone = plugins.create(kReceiver);
    set(*alone, "config", receiverConfiguration());
    alone->initialize();
    std::string problems;
    for (const std::string &problem : alone->validate())
        problems += problem;
    report("validate() with nothing connected", problems,
           "transfer_id: Use a direct scalar interval-transfer port");
}

//! The messages ModelInitializer gives for documents that cannot apply.
void compositions(const Plugins &plugins)
{
    std::cout << "\n[compositions] errors\n";
    auto link = [](const std::string &from, const std::string &output,
                   const std::string &to, const std::string &input) {
        return Json{{"from", {{"component", from}, {"output", output}}},
                    {"to", {{"component", to}, {"input", input}}}};
    };
    const Json meshAndGain =
        Json::array({block("meshgen", kMesh), block("gain", kGain)});

    report("unknown component in a connection",
           applyDocument(plugins,
                 {{"components", meshAndGain},
                  {"connections",
                   Json::array({link("meshgen", "mesh", "solver", "in")})}}),
           "is not declared in components[]");
    report("unknown output",
           applyDocument(plugins,
                 {{"components", meshAndGain},
                  {"connections",
                   Json::array({link("meshgen", "grid", "gain", "in")})}}),
           "has no output \"grid\"");
    report("unknown input",
           applyDocument(plugins,
                 {{"components", meshAndGain},
                  {"connections",
                   Json::array({link("meshgen", "mesh", "gain", "input")})}}),
           "has no input \"input\"");
    report("geometry output to a scalar input",
           applyDocument(plugins,
                 {{"components", meshAndGain},
                  {"connections",
                   Json::array({link("meshgen", "mesh", "gain", "in")})}}),
           "Provider is not one-dimensional");
    report("String output to a Float64 input",
           applyDocument(
               plugins,
               {{"components",
                 Json::array({block("donor", kSource,
                                    {{"config", sourceConfiguration()}}),
                              block("gain", kGain)})},
                {"connections",
                 Json::array({link("donor", "transfer_id", "gain", "in")})}}),
           "Provider data kind mismatch");
    Json roleOnPlainInput = {
        {"components",
         Json::array({block("source", kSignal), block("gain", kGain)})},
        {"connections",
         Json::array({link("source", "signal", "gain", "in")})}};
    roleOnPlainInput["connections"][0]["to"]["role"] = "augend";
    report("to.role on an input that is not a multi-input",
           applyDocument(plugins, roleOnPlainInput), "");
    report("unknown argument",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("gain", kGain, {{"scale", values(2)}})})}}),
           "has no argument \"scale\"");
    report("argument of the wrong type",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("gain", kGain, {{"gain", values("x")}})})}}),
           "type must be number");
    report("component that fails to initialize",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("sink", kRecorder)})}}),
           "failed to reach Initialized");
    auto bind = [](const std::string &component, const std::string &output) {
        return Json{{"@from", {{"component", component}, {"output", output}}}};
    };
    report("binding to an output the provider lacks",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("source", kSignal),
                                block("gain", kGain,
                                      {{"bias", bind("source", "value")}})})}}),
           "has no output \"value\"");
    report("binding to an output of the wrong shape",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("meshgen", kMesh),
                                block("gain", kGain,
                                      {{"bias", bind("meshgen", "mesh")}})})}}),
           "rejected the bound output");
    report("binding to a provider that fails validation",
           applyDocument(
               plugins,
               {{"components",
                 Json::array(
                     {block("meshgen", kMesh,
                            {{"quad_quality", values(2.0)}}),
                      block("gain", kGain, {{"bias", bind("meshgen", "mesh")}})})}}),
           "failed validation");
    report("binding to a component the document does not declare",
           applyDocument(plugins,
                 {{"components",
                   Json::array({block("gain", kGain,
                                      {{"bias", bind("calibration", "signal")}})})}}),
           "calibration");
}

} // namespace

int main()
{
    std::cout << "host stamp: " << HYDROCOUPLE_COMPONENT_ABI_STAMP << '\n';
    {
        Plugins plugins;
        smoke(plugins);
        signalSource(plugins);
        gain(plugins);
        recorder(plugins);
        combiner(plugins);
        basicAdapters(plugins);
        meshGenerator(plugins);
        timeSeries(plugins);
        interval(plugins);
        compositions(plugins);
    }
    std::cout << (g_failures == 0 ? "\nOK\n" : "\nFAILED\n");
    return g_failures == 0 ? 0 : 1;
}
