// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The standalone adapter-factory plugin's gates (CONNECT D3): it loads
// through the stamped ABI as a FACTORY (not a model component), and its
// negate and clamp adapters transform and CHAIN over rank-1 outputs.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <dlfcn.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple::SDK;

namespace {

std::filesystem::path libraryPath()
{
    std::filesystem::path path = std::filesystem::path{HCC_COMPONENTS_DIR} /
                                 "libhcc_basicadapters.so";
    if (!std::filesystem::exists(path))
    {
        path.replace_extension(".dylib");
    }
    return path;
}

std::vector<double> readAll(const HydroCouple::IComponentDataItem &item)
{
    const std::vector<int64_t> shape = item.shape();
    std::vector<double> values(static_cast<size_t>(shape[0]));

    HydroCouple::BufferDescriptor destination;
    destination.data = values.data();
    destination.kind = HydroCouple::DataKind::Float64;
    destination.rank = 1;
    destination.shape = shape.data();

    const int64_t start = 0;
    std::string message;
    EXPECT_TRUE(item.getValuesInto(destination,
                                   std::span<const int64_t>(&start, 1),
                                   std::span<const int64_t>(&shape[0], 1),
                                   &message))
        << message;
    return values;
}

class BasicAdaptersTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        const std::filesystem::path path = libraryPath();
        ASSERT_TRUE(std::filesystem::exists(path)) << path;
        s_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        ASSERT_NE(s_handle, nullptr) << dlerror();
    }

    static void TearDownTestSuite()
    {
        if (s_handle)
        {
            dlclose(s_handle);
        }
        s_handle = nullptr;
    }

    static void *s_handle;
};

void *BasicAdaptersTest::s_handle = nullptr;

} // namespace

TEST_F(BasicAdaptersTest, AdapterFactoryPluginLoadsThroughTheStampedAbi)
{
    auto abi = reinterpret_cast<HydroCoupleComponentAbiFn>(
        dlsym(s_handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
    ASSERT_NE(abi, nullptr) << dlerror();
    EXPECT_STREQ(abi(), HYDROCOUPLE_COMPONENT_ABI_STAMP);

    auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
        dlsym(s_handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
    ASSERT_NE(info, nullptr) << dlerror();

    HydroCouple::IComponentInfo *raw = info();
    ASSERT_NE(raw, nullptr);
    EXPECT_EQ(raw->id(), "hydrocouple.components.adapters.basic");

    // It answers as a FACTORY info across the image boundary — and NOT as a
    // model component info, which is what keeps it off the canvas.
    auto *factoryInfo =
        dynamic_cast<HydroCouple::IAdaptedOutputFactoryComponentInfo *>(raw);
    ASSERT_NE(factoryInfo, nullptr)
        << "the cross-image cast to the factory-info interface failed";
    EXPECT_EQ(dynamic_cast<HydroCouple::IModelComponentInfo *>(raw), nullptr)
        << "a factory info that also answers as a model component would be "
           "placeable on the canvas";

    std::unique_ptr<HydroCouple::IAdaptedOutputFactoryComponent> factory =
        factoryInfo->createComponentInstance();
    ASSERT_NE(factory, nullptr);
    EXPECT_EQ(factory->id(), "hydrocouple.components.adapters.basic")
        << "the instance id must match what a composition's 'factory' "
           "field names";
    EXPECT_EQ(factory->componentInfo(), factoryInfo);
}

TEST_F(BasicAdaptersTest, NegateAndClampAdaptChainableOneDimensionalOutputs)
{
    auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
        dlsym(s_handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
    ASSERT_NE(info, nullptr);
    auto *factoryInfo =
        dynamic_cast<HydroCouple::IAdaptedOutputFactoryComponentInfo *>(
            info());
    ASSERT_NE(factoryInfo, nullptr);

    std::unique_ptr<HydroCouple::IAdaptedOutputFactoryComponent> factory =
        factoryInfo->createComponentInstance();
    ASSERT_NE(factory, nullptr);

    Dimension scalar{"i"};
    std::unique_ptr<Quantity> quantity{Quantity::unitLess("Q")};
    Output1DDouble produced("produced", &scalar, 3, quantity.get(), nullptr);
    produced[0] = 3.0;
    produced[1] = -8.0;
    produced[2] = 5.0;

    const std::vector<HydroCouple::IIdentity *> offerings =
        factory->getAvailableAdaptedOutputIds(&produced);
    ASSERT_EQ(offerings.size(), 2u);
    EXPECT_EQ(offerings[0]->id(), "negate");
    EXPECT_EQ(offerings[1]->id(), "clamp");

    std::unique_ptr<HydroCouple::IAdaptedOutput> negate =
        factory->createAdaptedOutput(offerings[0], &produced);
    ASSERT_NE(negate, nullptr);
    negate->initialize();
    EXPECT_EQ(readAll(*negate), (std::vector<double>{-3.0, 8.0, -5.0}));

    // Chained: the clamp adapts the NEGATE's output — an adapted output is
    // itself an IOutput, and the factory must offer for it too.
    const std::vector<HydroCouple::IIdentity *> chained =
        factory->getAvailableAdaptedOutputIds(negate.get());
    ASSERT_EQ(chained.size(), 2u);

    std::unique_ptr<HydroCouple::IAdaptedOutput> clamp =
        factory->createAdaptedOutput(chained[1], negate.get());
    ASSERT_NE(clamp, nullptr);

    std::string message;
    for (HydroCouple::IArgument *argument : clamp->arguments())
    {
        if (argument && argument->id() == "minimum")
        {
            ASSERT_TRUE(argument->initialize(
                R"({"values": [-4.0]})",
                HydroCouple::IArgument::ArgumentInputType::JSON, message))
                << message;
        }
        if (argument && argument->id() == "maximum")
        {
            ASSERT_TRUE(argument->initialize(
                R"({"values": [4.0]})",
                HydroCouple::IArgument::ArgumentInputType::JSON, message))
                << message;
        }
    }
    clamp->initialize();
    EXPECT_EQ(readAll(*clamp), (std::vector<double>{-3.0, 4.0, -4.0}));

    // The provider's refresh fan-out reaches through the whole chain.
    produced[0] = 100.0;
    produced[1] = 0.5;
    produced[2] = -100.0;
    negate->refresh();
    EXPECT_EQ(readAll(*clamp), (std::vector<double>{-4.0, -0.5, 4.0}));

    // Non-owning registries: unregister before destruction, inner first.
    EXPECT_TRUE(negate->removeAdaptedOutput(clamp.get()));
    EXPECT_TRUE(produced.removeAdaptedOutput(negate.get()));
}
