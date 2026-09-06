// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The combiner's gates (CONNECT D2): the ecosystem's first concrete
// AbstractMultiInput — providers land under NAMED ROLES, unknown roles are
// refused by name, and the combination is asserted by value.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/core/identity.h"

#include <dlfcn.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using HydroCouple::IModelComponent;
using ComponentStatus = HydroCouple::IModelComponent::ComponentStatus;
using AT = HydroCouple::IArgument::ArgumentInputType;

namespace {

std::filesystem::path libraryPath(const std::string &stem)
{
    std::filesystem::path path =
        std::filesystem::path{HCC_COMPONENTS_DIR} / ("lib" + stem + ".so");
    if (!std::filesystem::exists(path))
    {
        path.replace_extension(".dylib");
    }
    return path;
}

void setArgument(IModelComponent &component, const std::string &argumentId,
                 const std::string &json)
{
    for (HydroCouple::IArgument *argument : component.arguments())
    {
        if (argument && argument->id() == argumentId)
        {
            std::string message;
            ASSERT_TRUE(argument->initialize(json, AT::JSON, message))
                << argumentId << ": " << message;
            return;
        }
    }
    FAIL() << "no argument \"" << argumentId << "\"";
}

double scalarOf(const HydroCouple::IComponentDataItem &item)
{
    double value = 0.0;
    const int64_t one = 1;
    HydroCouple::BufferDescriptor destination;
    destination.data = &value;
    destination.kind = HydroCouple::DataKind::Float64;
    destination.rank = 1;
    destination.shape = &one;
    const int64_t start = 0;
    std::string message;
    EXPECT_TRUE(item.getValuesInto(destination,
                                   std::span<const int64_t>(&start, 1),
                                   std::span<const int64_t>(&one, 1),
                                   &message))
        << message;
    return value;
}

class CombinerTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        for (const std::string stem : {"hcc_signalsource", "hcc_combiner"})
        {
            const std::filesystem::path path = libraryPath(stem);
            ASSERT_TRUE(std::filesystem::exists(path))
                << stem << " not built at " << path;

            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            ASSERT_NE(handle, nullptr) << dlerror();
            s_handles.push_back(handle);

            auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
                dlsym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
            ASSERT_NE(info, nullptr) << dlerror();
            auto *modelInfo =
                dynamic_cast<HydroCouple::IModelComponentInfo *>(info());
            ASSERT_NE(modelInfo, nullptr);
            s_infos.push_back(modelInfo);
        }
    }

    static void TearDownTestSuite()
    {
        s_infos.clear();
        for (void *handle : s_handles)
        {
            dlclose(handle);
        }
        s_handles.clear();
    }

    //! A constant source producing `value` for `steps` steps, initialized.
    static std::unique_ptr<IModelComponent> constantSource(double value,
                                                           int steps)
    {
        std::unique_ptr<IModelComponent> source =
            s_infos[0]->createComponentInstance();
        source->initialize();
        setArgument(*source, "waveform", R"({"values": ["constant"]})");
        setArgument(*source, "offset", "{\"values\": [" +
                                           std::to_string(value - 1.0) +
                                           "]}");
        setArgument(*source, "steps",
                    "{\"values\": [" + std::to_string(steps) + "]}");
        // amplitude keeps its default 1.0; constant = offset + amplitude.
        source->initialize();
        return source;
    }

    static std::unique_ptr<IModelComponent> combiner(
        const std::string &operation)
    {
        std::unique_ptr<IModelComponent> component =
            s_infos[1]->createComponentInstance();
        component->initialize();
        setArgument(*component, "operation",
                    "{\"values\": [\"" + operation + "\"]}");
        component->initialize();
        return component;
    }

    static std::vector<void *> s_handles;
    static std::vector<HydroCouple::IModelComponentInfo *> s_infos;
};

std::vector<void *> CombinerTest::s_handles;
std::vector<HydroCouple::IModelComponentInfo *> CombinerTest::s_infos;

} // namespace

TEST_F(CombinerTest, CombinerRefusesAProviderForAnUnknownRole)
{
    std::unique_ptr<IModelComponent> source = constantSource(3.0, 1);
    std::unique_ptr<IModelComponent> sums = combiner("sum");

    auto *operands =
        dynamic_cast<HydroCouple::IMultiInput *>(sums->inputs()[0]);
    ASSERT_NE(operands, nullptr)
        << "the combiner's input does not answer as an IMultiInput";

    // The roles are discoverable, both required.
    const std::vector<HydroCouple::IIdentity *> labels =
        operands->providerLabels();
    ASSERT_EQ(labels.size(), 2u);
    EXPECT_EQ(labels[0]->id(), "augend");
    EXPECT_EQ(labels[1]->id(), "addend");
    EXPECT_TRUE(operands->isRequiredProvider(labels[0]));

    HydroCouple::IOutput *out = source->outputs()[0];
    std::string message;

    // Role-less consumption is refused, and so is a role nothing declared —
    // each naming the roles that DO exist, because the fixer's next move is
    // to pick one.
    EXPECT_FALSE(operands->canConsume(out, message));
    EXPECT_NE(message.find("augend"), std::string::npos) << message;

    HydroCouple::SDK::Identity stranger("multiplicand", "Stranger");
    EXPECT_FALSE(operands->canConsume(out, message, &stranger));
    EXPECT_NE(message.find("multiplicand"), std::string::npos) << message;
    EXPECT_NE(message.find("addend"), std::string::npos) << message;

    HydroCouple::SDK::Identity augend("augend", "Augend");
    EXPECT_TRUE(operands->canConsume(out, message, &augend)) << message;
}

TEST_F(CombinerTest, CombinerBindsProvidersByRoleAndCombinesThem)
{
    std::unique_ptr<IModelComponent> first = constantSource(3.0, 2);
    std::unique_ptr<IModelComponent> second = constantSource(4.0, 2);
    std::unique_ptr<IModelComponent> sums = combiner("sum");

    auto *operands =
        dynamic_cast<HydroCouple::IMultiInput *>(sums->inputs()[0]);
    ASSERT_NE(operands, nullptr);

    HydroCouple::SDK::Identity augend("augend", "Augend");
    HydroCouple::SDK::Identity addend("addend", "Addend");
    ASSERT_TRUE(operands->addProvider(first->outputs()[0], &augend));
    ASSERT_TRUE(operands->addProvider(second->outputs()[0], &addend));

    for (IModelComponent *component :
         {first.get(), second.get(), sums.get()})
    {
        ASSERT_TRUE(component->validate().empty());
        component->prepare();
        ASSERT_NE(component->status(), ComponentStatus::Failed);
    }

    // Step 1: both providers still running → the combiner keeps going.
    first->update();
    second->update();
    sums->update();
    ASSERT_NE(sums->status(), ComponentStatus::Failed);
    EXPECT_NEAR(scalarOf(*sums->outputs()[0]), 7.0, 1.0e-9);
    EXPECT_EQ(sums->status(), ComponentStatus::Updated)
        << "the combiner outran its providers";

    // Step 2: both Done → the combiner follows.
    first->update();
    second->update();
    sums->update();
    EXPECT_EQ(sums->status(), ComponentStatus::Done);

    // The mean is a different operation, not a renamed sum.
    std::unique_ptr<IModelComponent> thirds = constantSource(3.0, 1);
    std::unique_ptr<IModelComponent> fourths = constantSource(4.0, 1);
    std::unique_ptr<IModelComponent> means = combiner("mean");
    auto *meanOperands =
        dynamic_cast<HydroCouple::IMultiInput *>(means->inputs()[0]);
    ASSERT_TRUE(meanOperands->addProvider(thirds->outputs()[0], &augend));
    ASSERT_TRUE(meanOperands->addProvider(fourths->outputs()[0], &addend));
    for (IModelComponent *component :
         {thirds.get(), fourths.get(), means.get()})
    {
        component->prepare();
        ASSERT_NE(component->status(), ComponentStatus::Failed);
    }
    thirds->update();
    fourths->update();
    means->update();
    EXPECT_NEAR(scalarOf(*means->outputs()[0]), 3.5, 1.0e-9);
}
