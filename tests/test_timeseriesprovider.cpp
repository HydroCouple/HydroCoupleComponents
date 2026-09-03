// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// Gates for TimeSeriesProviderComponent: each format reader against a
// fixture whose right answer is computed independently, the refusal paths
// by name, and the pipeline gate — a consumer's TimeSeriesArgumentDouble
// bound to a provider output through @from.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/io/modelinitializer.h"
#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/temporal/timedata.h"
#include "hydrocouplesdk/data/bufferio.h"
#include "hydrocouplesdk/io/uriresolver.h"
#include "hydrocouplesdk/temporal/timeseriesargument.h"

#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace {

std::string fixture(const std::string &name)
{
    return (std::filesystem::path(HCC_FIXTURES_DIR) / "timeseries" / name)
        .string();
}

class TimeSeriesProviderTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        std::filesystem::path path =
            std::filesystem::path(HCC_COMPONENTS_DIR) /
            "libhcc_timeseriesprovider.so";
        if (!std::filesystem::exists(path))
            path.replace_extension(".dylib");
        s_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(s_handle, nullptr) << dlerror();
        auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
            dlsym(s_handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
        ASSERT_NE(info, nullptr) << dlerror();
        s_info = dynamic_cast<IModelComponentInfo *>(info());
        ASSERT_NE(s_info, nullptr);
    }

    static void TearDownTestSuite()
    {
        s_info = nullptr;
        if (s_handle)
            dlclose(s_handle);
        s_handle = nullptr;
    }

    //! Initializes an instance over \p sourcePath; nullptr with the
    //! component's message in \p failure when initialization fails.
    static std::unique_ptr<IModelComponent> openSource(
        const std::string &sourcePath, std::string *failure = nullptr)
    {
        using Status = IModelComponent::ComponentStatus;
        std::unique_ptr<IModelComponent> component =
            s_info->createComponentInstance();
        std::string message;
        for (IArgument *argument : component->arguments())
        {
            if (argument->id() == "source")
                EXPECT_TRUE(argument->initialize(
                    nlohmann::json{{"values", {sourcePath}}}.dump(),
                    IArgument::ArgumentInputType::JSON, message))
                    << message;
        }
        component->initialize();
        if (component->status() != Status::Initialized)
        {
            if (failure)
                for (const auto &entry : component->errors())
                    *failure += entry.message + "\n";
            return nullptr;
        }
        return component;
    }

    static const HydroCouple::Temporal::ITimeSeriesComponentDataItem *series(
        IModelComponent &component, const std::string &id)
    {
        for (IOutput *output : component.outputs())
        {
            if (output->id() == id)
                return dynamic_cast<
                    const HydroCouple::Temporal::
                        ITimeSeriesComponentDataItem *>(output);
        }
        return nullptr;
    }

    //! A series' scalar values, read through the typed data plane — the
    //! same path any consumer copies through.
    static std::vector<double> valuesOf(
        const HydroCouple::Temporal::ITimeSeriesComponentDataItem &item)
    {
        const auto shape = item.shape();
        std::vector<double> values(
            static_cast<size_t>(shape.empty() ? 0 : shape[0]), -999.0);
        if (values.empty())
            return values;
        const BufferDescriptor destination = SDK::detail::hostDescriptor(
            values.data(), DataKind::Float64, shape);
        const std::vector<int64_t> zero(shape.size(), 0);
        std::string message;
        EXPECT_TRUE(item.getValuesInto(destination, zero, shape, &message))
            << message;
        return values;
    }

    static void *s_handle;
    static IModelComponentInfo *s_info;
};

void *TimeSeriesProviderTest::s_handle = nullptr;
IModelComponentInfo *TimeSeriesProviderTest::s_info = nullptr;

} // namespace

TEST_F(TimeSeriesProviderTest, CsvColumnsBecomeOutputsWithParsedDatetimes)
{
    auto component = openSource(fixture("flows.csv"));
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->outputs().size(), 2u);

    const auto *flow = series(*component, "flow");
    ASSERT_NE(flow, nullptr);
    ASSERT_EQ(flow->timeCount(), 3);
    const double newYear = SDK::Temporal::TimeData::gregorianToJulianDay(
        2020, 1, 1, 0, 0, 0.0);
    EXPECT_DOUBLE_EQ(flow->times()[0], newYear);
    EXPECT_DOUBLE_EQ(flow->times()[1], newYear + 0.25);
    EXPECT_DOUBLE_EQ(valuesOf(*flow)[0], 1.5);
    EXPECT_DOUBLE_EQ(valuesOf(*flow)[2], 3.5);

    const auto *stage = series(*component, "stage");
    ASSERT_NE(stage, nullptr);
    EXPECT_DOUBLE_EQ(valuesOf(*stage)[1], 10.5);
}

TEST_F(TimeSeriesProviderTest, ANumericFirstColumnIsTakenAsTimeDirectly)
{
    auto component = openSource(fixture("plain.csv"));
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->outputs().size(), 1u);
    const auto *only = dynamic_cast<
        const HydroCouple::Temporal::ITimeSeriesComponentDataItem *>(
        component->outputs()[0]);
    ASSERT_NE(only, nullptr);
    ASSERT_EQ(only->timeCount(), 2);
    EXPECT_DOUBLE_EQ(only->times()[0], 2458849.5);
    EXPECT_DOUBLE_EQ(valuesOf(*only)[1], 8.75);
}

TEST_F(TimeSeriesProviderTest, W2NptColumnsCarryTheirHeaderNames)
{
    auto component = openSource(fixture("met.npt"));
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->outputs().size(), 2u);

    const auto *air = series(*component, "TAIR");
    ASSERT_NE(air, nullptr);
    ASSERT_EQ(air->timeCount(), 3);
    EXPECT_DOUBLE_EQ(air->times()[0], 1.0);
    EXPECT_DOUBLE_EQ(air->times()[2], 2.0);
    EXPECT_DOUBLE_EQ(valuesOf(*air)[0], 4.5);

    const auto *dew = series(*component, "TDEW");
    ASSERT_NE(dew, nullptr);
    EXPECT_DOUBLE_EQ(valuesOf(*dew)[2], 1.5);
}

TEST_F(TimeSeriesProviderTest, ASwmmDatCarriesTheDateForward)
{
    auto component = openSource(fixture("rain.dat"));
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->outputs().size(), 1u);

    const auto *rain = series(*component, "rain");
    ASSERT_NE(rain, nullptr);
    ASSERT_EQ(rain->timeCount(), 4);
    const double day = SDK::Temporal::TimeData::gregorianToJulianDay(
        2020, 1, 31, 0, 0, 0.0);
    EXPECT_DOUBLE_EQ(rain->times()[0], day);
    EXPECT_DOUBLE_EQ(rain->times()[1], day + 0.25); // 06:00, date carried
    EXPECT_DOUBLE_EQ(rain->times()[3], day + 1.0);  // the next day's row
    EXPECT_DOUBLE_EQ(valuesOf(*rain)[1], 0.25);
    EXPECT_DOUBLE_EQ(valuesOf(*rain)[3], 0.1);
}

TEST_F(TimeSeriesProviderTest, AnUnknownExtensionFailsNamingWhatWasTried)
{
    std::string failure;
    auto component = openSource(fixture("mystery.xyz"), &failure);
    EXPECT_EQ(component, nullptr);
    EXPECT_NE(failure.find(".csv"), std::string::npos) << failure;
    EXPECT_NE(failure.find("format"), std::string::npos) << failure;
}

TEST_F(TimeSeriesProviderTest, NetcdfIsRefusedWithDirectionsNotMisread)
{
    std::string failure;
    auto component = openSource(fixture("anything.nc"), &failure);
    EXPECT_EQ(component, nullptr);
    EXPECT_NE(failure.find("NetCDF"), std::string::npos) << failure;
}

TEST_F(TimeSeriesProviderTest, ASeriesArgumentBindsToAProvidersOutput)
{
    class Consumer : public AbstractModelComponent
    {
      public:
        explicit Consumer(std::string_view id) : AbstractModelComponent(id)
        {}
        std::vector<std::string> validate() override { return {}; }
        void prepare() override {}
        void update(const std::vector<IOutput *> & = {}) override {}
        void finish() override {}
        SDK::Temporal::TimeSeriesArgumentDouble *met = nullptr;

      protected:
        void createArguments() override
        {
            met = new SDK::Temporal::TimeSeriesArgumentDouble(
                "met", "Met forcing", this, &m_timeDimension);
            addArgument(met);
        }
        bool initializeArguments(std::string &message) override
        {
            message.clear();
            return true;
        }
        void createInputs() override {}
        void createOutputs() override {}
        void initializeFailureCleanUp() override {}

      private:
        Dimension m_timeDimension{"time", "Time"};
    };

    Consumer consumer("solver");
    std::unique_ptr<IModelComponent> provider =
        s_info->createComponentInstance();
    IO::ModelInitializer initializer(
        [&](const std::string &id) -> IModelComponent * {
            if (id == "solver")   return &consumer;
            if (id == "provider") return provider.get();
            return nullptr;
        });

    const nlohmann::json document = {
        {"components",
         {{{"id", "solver"},
           {"arguments",
            {{"met",
              {{"@from",
                {{"component", "provider"}, {"output", "TAIR"}}}}}}}},
          {{"id", "provider"},
           {"arguments",
            {{"source", {{"values", {fixture("met.npt")}}}}}}}}}};

    std::string message;
    ASSERT_TRUE(initializer.initializeFromJsonText(document.dump(), message))
        << message;

    ASSERT_NE(consumer.met, nullptr);
    ASSERT_NE(consumer.met->timeSeries(), nullptr);
    ASSERT_EQ(consumer.met->timeSeries()->count(), 3);
    EXPECT_DOUBLE_EQ(consumer.met->timeSeries()->values()[1], 6.0);
}

// ── USGS NWIS web sources (S2.2b) ─────────────────────────────────────────

namespace {

/*!
 * The transport a host would install, in miniature: serves one payload,
 * counts requests, and can be told to go offline.
 */
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
        ++requests;
        contents = payload;
        return true;
    }

    std::string payload;
    bool        offline  = false;
    int         requests = 0;
};

//! Installs a resolver for one test and always uninstalls it.
class ResolverGuard
{
  public:
    explicit ResolverGuard(IO::UriResolver *resolver)
    {
        IO::setUriResolver(resolver);
    }
    ~ResolverGuard() { IO::setUriResolver(nullptr); }
};

std::string fileText(const std::string &path)
{
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

constexpr const char *kNwisUrl =
    "https://waterservices.usgs.gov/nwis/iv/?sites=09380000&format=json";

} // namespace

TEST_F(TimeSeriesProviderTest, NwisSeriesAreNamedSiteAndParameter)
{
    StubResolver resolver;
    resolver.payload = fileText(fixture("nwis_iv.json"));
    ResolverGuard guard(&resolver);

    auto component = openSource(kNwisUrl);
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->outputs().size(), 2u);

    const auto *flow = series(*component, "09380000_00060");
    ASSERT_NE(flow, nullptr);
    ASSERT_EQ(flow->timeCount(), 3);
    EXPECT_DOUBLE_EQ(valuesOf(*flow)[0], 11400.0);
    EXPECT_DOUBLE_EQ(valuesOf(*flow)[2], 11350.0);
    // 06:00 is a quarter day after midnight, as the service stamped it.
    EXPECT_DOUBLE_EQ(flow->times()[1] - flow->times()[0], 0.25);

    const auto *stage = series(*component, "09380000_00065");
    ASSERT_NE(stage, nullptr);
    EXPECT_DOUBLE_EQ(valuesOf(*stage)[1], 9.90);
    EXPECT_EQ(resolver.requests, 1);
}

TEST_F(TimeSeriesProviderTest, AFetchIsCachedAndServedOffline)
{
    const std::string cachePath =
        (std::filesystem::path(HCC_COMPONENTS_DIR) / "nwis_cache.json")
            .string();
    std::filesystem::remove(cachePath);

    StubResolver resolver;
    resolver.payload = fileText(fixture("nwis_iv.json"));
    ResolverGuard guard(&resolver);

    auto setCache = [&](IModelComponent &component) {
        std::string message;
        for (IArgument *argument : component.arguments())
        {
            if (argument->id() == "cache")
                EXPECT_TRUE(argument->initialize(
                    nlohmann::json{{"values", {cachePath}}}.dump(),
                    IArgument::ArgumentInputType::JSON, message))
                    << message;
        }
    };

    // Online: the fetch lands in the cache file.
    {
        std::unique_ptr<IModelComponent> component =
            s_info->createComponentInstance();
        setCache(*component);
        std::string message;
        for (IArgument *argument : component->arguments())
            if (argument->id() == "source")
                ASSERT_TRUE(argument->initialize(
                    nlohmann::json{{"values", {kNwisUrl}}}.dump(),
                    IArgument::ArgumentInputType::JSON, message))
                    << message;
        component->initialize();
        ASSERT_EQ(component->status(),
                  IModelComponent::ComponentStatus::Initialized);
        ASSERT_TRUE(std::filesystem::exists(cachePath));
    }

    // Offline: the cached numbers are the run's numbers.
    resolver.offline = true;
    {
        std::unique_ptr<IModelComponent> component =
            s_info->createComponentInstance();
        setCache(*component);
        std::string message;
        for (IArgument *argument : component->arguments())
            if (argument->id() == "source")
                ASSERT_TRUE(argument->initialize(
                    nlohmann::json{{"values", {kNwisUrl}}}.dump(),
                    IArgument::ArgumentInputType::JSON, message))
                    << message;
        component->initialize();
        ASSERT_EQ(component->status(),
                  IModelComponent::ComponentStatus::Initialized);
        const auto *flow = series(*component, "09380000_00060");
        ASSERT_NE(flow, nullptr);
        EXPECT_DOUBLE_EQ(valuesOf(*flow)[0], 11400.0);
    }
    EXPECT_EQ(resolver.requests, 1) << "the offline run must not refetch";
}

TEST_F(TimeSeriesProviderTest, AnEmptyAnswerFailsPointingAtTheCodes)
{
    StubResolver resolver;
    resolver.payload = fileText(fixture("nwis_empty.json"));
    ResolverGuard guard(&resolver);

    std::string failure;
    auto component = openSource(kNwisUrl, &failure);
    EXPECT_EQ(component, nullptr);
    EXPECT_NE(failure.find("site and parameter"), std::string::npos)
        << failure;
}

TEST_F(TimeSeriesProviderTest, OfflineWithNoCacheFailsNamingBoth)
{
    StubResolver resolver;
    resolver.offline = true;
    ResolverGuard guard(&resolver);

    std::string failure;
    auto component = openSource(kNwisUrl, &failure);
    EXPECT_EQ(component, nullptr);
    EXPECT_NE(failure.find("cache"), std::string::npos) << failure;
    EXPECT_NE(failure.find("offline"), std::string::npos) << failure;
}
