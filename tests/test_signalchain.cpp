// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The testing family's gates (CONNECT D1): a deterministic source, the
// ecosystem's first shipped input-bearing transformer, and a CSV-writing
// sink — each loaded through the stamped ABI and driven the way a workflow
// drives them, wired output to input across library boundaries.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"

#include <dlfcn.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
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

//! Applies one JSON payload to a named argument.
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

//! Reads element 0 of a rank-1 Double item.
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

/*!
 * Opens the three testing-family plugins once for the suite and hands out
 * fresh instances; instances always die before the libraries that
 * allocated them.
 */
class SignalChainTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        for (const std::string stem :
             {"hcc_signalsource", "hcc_gaintransformer", "hcc_recorder"})
        {
            const std::filesystem::path path = libraryPath(stem);
            ASSERT_TRUE(std::filesystem::exists(path))
                << stem << " not built at " << path;

            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            ASSERT_NE(handle, nullptr) << dlerror();
            s_handles.push_back(handle);

            auto abi = reinterpret_cast<HydroCoupleComponentAbiFn>(
                dlsym(handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
            ASSERT_NE(abi, nullptr) << dlerror();
            ASSERT_STREQ(abi(), HYDROCOUPLE_COMPONENT_ABI_STAMP);

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

    static std::unique_ptr<IModelComponent> create(size_t index)
    {
        return s_infos[index]->createComponentInstance();
    }

    static constexpr size_t kSource = 0;
    static constexpr size_t kGain = 1;
    static constexpr size_t kRecorder = 2;

    static std::vector<void *> s_handles;
    static std::vector<HydroCouple::IModelComponentInfo *> s_infos;
};

std::vector<void *> SignalChainTest::s_handles;
std::vector<HydroCouple::IModelComponentInfo *> SignalChainTest::s_infos;

//! validate → prepare, asserting the component stays alive. Deliberately
//! NOT initialize: re-initializing recreates the exchange items and wipes
//! any wiring already made — callers initialize first, then wire, then this.
void bringUp(IModelComponent &component)
{
    ASSERT_TRUE(component.validate().empty());
    component.prepare();
    ASSERT_NE(component.status(), ComponentStatus::Failed);
}

} // namespace

TEST_F(SignalChainTest, SignalSourceProducesTheConfiguredWaveform)
{
    std::unique_ptr<IModelComponent> source = create(kSource);
    ASSERT_NE(source, nullptr);

    source->initialize();
    ASSERT_EQ(source->status(), ComponentStatus::Initialized);

    setArgument(*source, "waveform", R"({"values": ["sine"]})");
    setArgument(*source, "amplitude", R"({"values": [2.0]})");
    setArgument(*source, "frequency", R"({"values": [0.25]})");
    setArgument(*source, "offset", R"({"values": [10.0]})");
    setArgument(*source, "steps", R"({"values": [4]})");
    source->initialize(); // re-latch the edited arguments
    ASSERT_EQ(source->status(), ComponentStatus::Initialized);

    ASSERT_TRUE(source->validate().empty());
    source->prepare();
    ASSERT_NE(source->status(), ComponentStatus::Failed);

    ASSERT_EQ(source->outputs().size(), 1u);
    HydroCouple::IOutput *signal = source->outputs()[0];

    // sin at quarter cycle: 12, 10, 8, 10 — and Done exactly at steps.
    const double expected[4] = {12.0, 10.0, 8.0, 10.0};

    for (int step = 0; step < 4; ++step)
    {
        ASSERT_NE(source->status(), ComponentStatus::Done)
            << "done early, at step " << step;
        source->update();
        ASSERT_NE(source->status(), ComponentStatus::Failed);
        EXPECT_NEAR(scalarOf(*signal), expected[step], 1.0e-9)
            << "step " << step + 1;
    }

    EXPECT_EQ(source->status(), ComponentStatus::Done);
    source->finish();
    EXPECT_EQ(source->status(), ComponentStatus::Finished);
}

TEST_F(SignalChainTest, GainTransformerScalesItsInputEachStep)
{
    std::unique_ptr<IModelComponent> source = create(kSource);
    std::unique_ptr<IModelComponent> gain = create(kGain);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(gain, nullptr);

    source->initialize();
    setArgument(*source, "waveform", R"({"values": ["constant"]})");
    setArgument(*source, "amplitude", R"({"values": [3.0]})");
    setArgument(*source, "steps", R"({"values": [2]})");
    source->initialize();

    gain->initialize();
    setArgument(*gain, "gain", R"({"values": [10.0]})");
    setArgument(*gain, "bias", R"({"values": [-5.0]})");
    gain->initialize();

    // Wire across the library boundary, the way applyConnections does.
    HydroCouple::IOutput *out = source->outputs()[0];
    HydroCouple::IInput *in = gain->inputs()[0];
    ASSERT_TRUE(in->setProvider(out));
    out->addConsumer(in);

    ASSERT_NO_FATAL_FAILURE(bringUp(*source));
    ASSERT_NO_FATAL_FAILURE(bringUp(*gain));

    // Step 1: upstream not yet done → the transformer keeps going.
    source->update();
    gain->update();
    ASSERT_NE(gain->status(), ComponentStatus::Failed);
    EXPECT_NEAR(scalarOf(*gain->outputs()[0]), 10.0 * 3.0 - 5.0, 1.0e-9);
    EXPECT_EQ(gain->status(), ComponentStatus::Updated)
        << "the transformer outran its provider";

    // Step 2: the source reports Done; the transformer follows its clock.
    source->update();
    EXPECT_EQ(source->status(), ComponentStatus::Done);
    gain->update();
    EXPECT_EQ(gain->status(), ComponentStatus::Done);
}

TEST_F(SignalChainTest, RecorderWritesOneCsvRowPerStep)
{
    const std::filesystem::path artifacts{HCC_ARTIFACTS_DIR};
    std::filesystem::create_directories(artifacts);
    const std::filesystem::path recorded = artifacts / "signalchain.csv";
    std::filesystem::remove(recorded);

    std::unique_ptr<IModelComponent> source = create(kSource);
    std::unique_ptr<IModelComponent> recorder = create(kRecorder);

    source->initialize();
    setArgument(*source, "waveform", R"({"values": ["ramp"]})");
    setArgument(*source, "amplitude", R"({"values": [8.0]})");
    setArgument(*source, "frequency", R"({"values": [0.25]})");
    setArgument(*source, "steps", R"({"values": [3]})");
    source->initialize();

    recorder->initialize();
    // An empty path is refused by name: this sink exists to leave a file.
    EXPECT_EQ(recorder->status(), ComponentStatus::Failed);
    setArgument(*recorder, "path",
                std::string("{\"values\": [\"") + recorded.string() +
                    "\"]}");
    recorder->initialize();
    ASSERT_EQ(recorder->status(), ComponentStatus::Initialized);

    HydroCouple::IOutput *out = source->outputs()[0];
    HydroCouple::IInput *in = recorder->inputs()[0];
    ASSERT_TRUE(in->setProvider(out));
    out->addConsumer(in);

    ASSERT_NO_FATAL_FAILURE(bringUp(*source));
    ASSERT_NO_FATAL_FAILURE(bringUp(*recorder));

    int guard = 0;
    while (recorder->status() != ComponentStatus::Done && guard++ < 10)
    {
        source->update();
        recorder->update();
        ASSERT_NE(recorder->status(), ComponentStatus::Failed);
    }

    source->finish();
    recorder->finish();
    ASSERT_EQ(recorder->status(), ComponentStatus::Finished);

    // ramp at f=0.25: 8*0.25, 8*0.5, 8*0.75.
    std::ifstream file(recorded);
    ASSERT_TRUE(file.is_open()) << recorded;
    std::string line;
    ASSERT_TRUE(std::getline(file, line));
    EXPECT_EQ(line, "step,value");
    ASSERT_TRUE(std::getline(file, line));
    EXPECT_EQ(line, "1,2");
    ASSERT_TRUE(std::getline(file, line));
    EXPECT_EQ(line, "2,4");
    ASSERT_TRUE(std::getline(file, line));
    EXPECT_EQ(line, "3,6");
    EXPECT_FALSE(std::getline(file, line)) << "extra rows: " << line;
}
