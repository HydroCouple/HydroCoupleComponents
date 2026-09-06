// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// End-to-end gates over the committed composition fixtures (CONNECT D4):
// each document in compositions/ is parsed, applied through
// ModelInitializer (standalone adapter factories reaching it through the
// resolver seam), driven by TimeSteppedWorkflow, and judged by the CSV the
// recorder leaves behind. The same files are the manual smoke deck: point
// Composer's "Load Directory…" at build/components and open them.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/timesteppedworkflow.h"
#include "hydrocouplesdk/data/sdkadaptedoutputfactory.h"
#include "hydrocouplesdk/io/compositionspec.h"
#include "hydrocouplesdk/io/modelinitializer.h"

#include <dlfcn.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
using HydroCouple::SDK::IO::CompositionSpec;
using HydroCouple::SDK::IO::ModelInitializer;
using WStatus = IWorkflowComponent::WorkflowStatus;

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

/*!
 * Opens every testing-family plugin once for the suite: the model
 * components by info id, and the standalone adapter factory realized the
 * way a host realizes it.
 */
class CompositionsTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        for (const std::string stem :
             {"hcc_signalsource", "hcc_gaintransformer", "hcc_recorder",
              "hcc_combiner", "hcc_basicadapters"})
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

            IComponentInfo *raw = info();
            ASSERT_NE(raw, nullptr);

            if (auto *modelInfo =
                    dynamic_cast<IModelComponentInfo *>(raw))
            {
                s_modelInfos[modelInfo->id()] = modelInfo;
            }
            else if (auto *factoryInfo = dynamic_cast<
                         IAdaptedOutputFactoryComponentInfo *>(raw))
            {
                s_factory = factoryInfo->createComponentInstance();
                ASSERT_NE(s_factory, nullptr);
            }
        }

        ASSERT_EQ(s_modelInfos.size(), 4u);
        ASSERT_NE(s_factory, nullptr);

        s_artifacts = std::filesystem::path{HCC_ARTIFACTS_DIR};
        std::filesystem::create_directories(s_artifacts);
    }

    static void TearDownTestSuite()
    {
        s_factory.reset();
        s_modelInfos.clear();
        for (void *handle : s_handles)
        {
            dlclose(handle);
        }
        s_handles.clear();
    }

    /*!
     * Parses a fixture, realizes its components, applies it through
     * ModelInitializer with the standalone factory installed, runs the
     * time-stepped workflow to Done, and finishes every component.
     */
    void runComposition(const std::string &name)
    {
        const std::filesystem::path path =
            std::filesystem::path{HCC_COMPOSITIONS_DIR} / name;
        std::ifstream file(path);
        ASSERT_TRUE(file.is_open()) << path;
        std::stringstream text;
        text << file.rdbuf();

        std::string message;
        CompositionSpec spec;
        ASSERT_TRUE(CompositionSpec::parse(
            nlohmann::json::parse(text.str()), spec, message))
            << message;

        // Realize instances by the info id each component block names, and
        // anchor their relative paths at the artifacts directory so every
        // run leaves reviewable files.
        m_instances.clear();
        std::map<std::string, IModelComponent *> byId;

        for (const HydroCouple::SDK::IO::ComponentSpec &component :
             spec.components)
        {
            auto found = s_modelInfos.find(component.info.componentInfoId);
            ASSERT_NE(found, s_modelInfos.end())
                << component.id << " names unknown info "
                << component.info.componentInfoId;

            std::unique_ptr<IModelComponent> instance =
                found->second->createComponentInstance();
            ASSERT_NE(instance, nullptr);

            if (auto *abstract =
                    dynamic_cast<AbstractModelComponent *>(instance.get()))
            {
                abstract->setReferenceDirectory(s_artifacts.string());
            }

            byId[component.id] = instance.get();
            m_instances.push_back(std::move(instance));
        }

        ModelInitializer initializer(
            [&byId](const std::string &id) -> IModelComponent * {
                const auto it = byId.find(id);
                return it == byId.end() ? nullptr : it->second;
            });

        initializer.setAdapterFactoryResolver(
            []
            {
                return std::vector<IAdaptedOutputFactory *>{
                    SdkAdaptedOutputFactory::instance(), s_factory.get()};
            });

        ASSERT_TRUE(initializer.initializeFromJsonText(text.str(), message))
            << message;

        TimeSteppedWorkflow workflow("workflow");
        for (const std::unique_ptr<IModelComponent> &instance : m_instances)
        {
            ASSERT_TRUE(workflow.addModelComponent(instance.get(), nullptr,
                                                   &message))
                << message;
        }

        workflow.initialize();
        ASSERT_EQ(workflow.status(), WStatus::Initialized);
        ASSERT_TRUE(workflow.validate().empty());
        workflow.prepare();
        ASSERT_EQ(workflow.status(), WStatus::Prepared);

        int guard = 0;
        while (workflow.status() != WStatus::Done && ++guard < 20)
        {
            workflow.update();
        }
        ASSERT_EQ(workflow.status(), WStatus::Done)
            << "the composition never ran to Done";

        for (const std::unique_ptr<IModelComponent> &instance : m_instances)
        {
            instance->finish();
        }
    }

    //! The recorder's CSV, whole, as lines.
    static std::vector<std::string> recordedLines(const std::string &name)
    {
        std::ifstream file(s_artifacts / name);
        EXPECT_TRUE(file.is_open()) << (s_artifacts / name);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(file, line))
        {
            lines.push_back(line);
        }
        return lines;
    }

    std::vector<std::unique_ptr<IModelComponent>> m_instances;

    static std::vector<void *> s_handles;
    static std::map<std::string, IModelComponentInfo *> s_modelInfos;
    static std::unique_ptr<IAdaptedOutputFactoryComponent> s_factory;
    static std::filesystem::path s_artifacts;
};

std::vector<void *> CompositionsTest::s_handles;
std::map<std::string, IModelComponentInfo *> CompositionsTest::s_modelInfos;
std::unique_ptr<IAdaptedOutputFactoryComponent> CompositionsTest::s_factory;
std::filesystem::path CompositionsTest::s_artifacts;

} // namespace

TEST_F(CompositionsTest, SourceToRecorderCompositionRunsAndRecordsEveryStep)
{
    ASSERT_NO_FATAL_FAILURE(
        runComposition("source_to_recorder.json"));

    EXPECT_EQ(recordedLines("recorded_direct.csv"),
              (std::vector<std::string>{"step,value", "1,5", "2,5", "3,5"}));
}

TEST_F(CompositionsTest, AdaptedCompositionNegatesAndClampsThroughTheChain)
{
    // ramp 2,4,6 → negate → -2,-4,-6 → clamp[-5,-3] → -3,-4,-5 → 2x+1.
    // Wrong values here mean the STANDALONE factory was never consulted,
    // the steps applied out of order, or the chain never refreshed.
    ASSERT_NO_FATAL_FAILURE(
        runComposition("source_gain_recorder_adapted.json"));

    EXPECT_EQ(recordedLines("recorded_adapted.csv"),
              (std::vector<std::string>{"step,value", "1,-5", "2,-7",
                                        "3,-9"}));
}

TEST_F(CompositionsTest, CombinerCompositionSumsItsRoleProviders)
{
    ASSERT_NO_FATAL_FAILURE(runComposition("combiner_roles.json"));

    EXPECT_EQ(recordedLines("recorded_combined.csv"),
              (std::vector<std::string>{"step,value", "1,7", "2,7"}));
}

TEST_F(CompositionsTest, BoundArgumentCompositionRunsItsProviderToFinishedFirst)
{
    ASSERT_NO_FATAL_FAILURE(runComposition("bound_arguments.json"));

    // bias = the calibration source's final signal (100), applied through
    // the @from binding BEFORE the gain initialized; the run then adds it
    // to every sample.
    EXPECT_EQ(recordedLines("recorded_bound.csv"),
              (std::vector<std::string>{"step,value", "1,105", "2,105"}));
}
