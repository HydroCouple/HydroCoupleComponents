// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The composition cookbook's runner. It applies every document in
// compositions/ that runs with this repository's plugins alone, and the
// smoke.json beside it, the way a host does: one instance per component
// block, named after the block, with the working directory as each
// instance's reference directory. It steps the time-stepped workflow to
// Done, finishes every component, prints each component's final scalar
// outputs, the generated mesh's size, the file the run left behind and
// anything a component queued on errors(), and fails when a result differs
// from the cookbook's.

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplespatial.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/timesteppedworkflow.h"
#include "hydrocouplesdk/data/sdkadaptedoutputfactory.h"
#include "hydrocouplesdk/io/compositionspec.h"
#include "hydrocouplesdk/io/modelinitializer.h"

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
using WorkflowStatus = IWorkflowComponent::WorkflowStatus;

namespace
{

//! The plugins every runnable composition names, opened once.
class Plugins
{
  public:
    Plugins()
    {
        for (const char *stem :
             {"hcc_smoke", "hcc_signalsource", "hcc_gaintransformer",
              "hcc_recorder", "hcc_combiner", "hcc_basicadapters",
              "hcc_meshgenerator", "hcc_intervalsource",
              "hcc_intervalreceiver"})
        {
            fs::path path = fs::path(HCC_COMPONENTS_DIR) /
                            (std::string("lib") + stem + ".so");
            if (!fs::exists(path))
                path.replace_extension(".dylib");
            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            auto abi = handle ? reinterpret_cast<HydroCoupleComponentAbiFn>(
                                    dlsym(handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL))
                              : nullptr;
            if (!abi || std::string(abi()) != HYDROCOUPLE_COMPONENT_ABI_STAMP)
                throw std::runtime_error("cannot load " + path.string());
            m_handles.push_back(handle);
            IComponentInfo *info =
                reinterpret_cast<HydroCoupleComponentInfoFn>(
                    dlsym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL))();
            if (auto *factoryInfo =
                    dynamic_cast<IAdaptedOutputFactoryComponentInfo *>(info))
                m_factory = factoryInfo->createComponentInstance();
            else
                m_infos[info->id()] = dynamic_cast<IModelComponentInfo *>(info);
        }
    }

    ~Plugins()
    {
        m_factory.reset();
        for (void *handle : m_handles)
            dlclose(handle);
    }

    IModelComponentInfo *info(const std::string &id) const
    {
        const auto found = m_infos.find(id);
        return found == m_infos.end() ? nullptr : found->second;
    }

    IAdaptedOutputFactory *factory() const { return m_factory.get(); }

  private:
    std::vector<void *> m_handles;
    std::map<std::string, IModelComponentInfo *> m_infos;
    std::unique_ptr<IAdaptedOutputFactoryComponent> m_factory;
};

//! Element 0 of a rank-1 Float64 item of length 1.
double scalarOf(const IOutput &output)
{
    double value = 0.0;
    const int64_t one = 1, zero = 0;
    const BufferDescriptor buffer{&value, DataKind::Float64, 1, &one,
                                  nullptr, 0};
    (void)output.getValuesInto(buffer, {&zero, 1}, {&one, 1});
    return value;
}

//! Prints the final value of every scalar output, and a mesh's size.
//! Returns the printed lines for checking.
std::string printOutputs(const IModelComponent &component)
{
    std::ostringstream text;
    for (IOutput *output : component.outputs())
    {
        if (auto *surface = dynamic_cast<
                Spatial::IPolyhedralSurfaceComponentDataItem *>(output);
            surface && surface->polyhedralSurface())
        {
            const auto *view = surface->polyhedralSurface()->meshView();
            text << "  " << component.id() << '.' << output->id() << ": "
                 << view->nodeCount() << " nodes, " << view->faceCount()
                 << " faces\n";
        }
        else if (output->dataKind() == DataKind::Float64 &&
                 output->shape() == std::vector<int64_t>{1})
        {
            text << "  " << component.id() << '.' << output->id() << " = "
                 << scalarOf(*output) << '\n';
        }
    }
    std::cout << text.str();
    return text.str();
}

std::string fileText(const fs::path &path)
{
    std::ifstream file(path);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

//! Runs one composition document; returns false with a message on failure.
//! \p outputs receives the printed final outputs.
bool runComposition(const Plugins &plugins, const fs::path &document,
                    std::string &outputs, std::string &message)
{
    IO::CompositionSpec spec;
    if (!IO::CompositionSpec::parse(nlohmann::json::parse(fileText(document)),
                                    spec, message))
        return false;

    // Instances live in this scope, narrower than the libraries'.
    std::vector<std::unique_ptr<IModelComponent>> instances;
    std::map<std::string, IModelComponent *> byId;
    for (const IO::ComponentSpec &block : spec.components)
    {
        IModelComponentInfo *info = plugins.info(block.info.componentInfoId);
        if (!info)
        {
            message = "no plugin provides " + block.info.componentInfoId;
            return false;
        }
        instances.push_back(info->createComponentInstance());
        auto *sdk = dynamic_cast<AbstractModelComponent *>(instances.back().get());
        if (!sdk->setInstanceId(block.id, message))
            return false;
        sdk->setReferenceDirectory(fs::current_path().string());
        byId[block.id] = instances.back().get();
    }

    IO::ModelInitializer initializer(
        [&byId](const std::string &id) -> IModelComponent * {
            const auto found = byId.find(id);
            return found == byId.end() ? nullptr : found->second;
        });
    initializer.setAdapterFactoryResolver([&plugins] {
        return std::vector<IAdaptedOutputFactory *>{
            SdkAdaptedOutputFactory::instance(), plugins.factory()};
    });
    if (!initializer.initializeFromFile(document, message))
        return false;

    TimeSteppedWorkflow workflow("workflow");
    for (const auto &instance : instances)
        if (!workflow.addModelComponent(instance.get(), nullptr, &message))
            return false;
    workflow.initialize();
    const std::vector<std::string> problems = workflow.validate();
    if (!problems.empty())
    {
        message = problems.front();
        return false;
    }
    workflow.prepare();
    int steps = 0;
    while (workflow.status() != WorkflowStatus::Done && steps < 100)
    {
        workflow.update();
        ++steps;
    }
    for (const auto &instance : instances)
        instance->finish();
    std::cout << "  workflow: " << steps << " updates, "
              << (workflow.status() == WorkflowStatus::Done ? "Done"
                                                            : "not Done")
              << '\n';

    for (const auto &instance : instances)
        outputs += printOutputs(*instance);
    for (const auto &instance : instances)
        for (const ErrorEntry &entry : instance->errors())
            std::cout << "  errors() of " << instance->id() << ": "
                      << entry.message << '\n';
    return workflow.status() == WorkflowStatus::Done;
}

} // namespace

int main()
{
    const fs::path compositions(HCC_COMPOSITIONS_DIR);
    const fs::path here(DOCS_EXAMPLE_SOURCE_DIR);

    // Each document, the file its run leaves (if any), what that file must
    // hold, and a line its final outputs must contain.
    struct Run
    {
        fs::path document;
        std::string file;
        std::string contents;
        std::string output;
    };
    const std::vector<Run> runs = {
        {here / "smoke.json", "", "", "smoke.produced = 42"},
        {compositions / "source_to_recorder.json", "recorded_direct.csv",
         "step,value\n1,5\n2,5\n3,5\n", "source.signal = 5"},
        {compositions / "source_gain_recorder_adapted.json",
         "recorded_adapted.csv", "step,value\n1,-5\n2,-7\n3,-9\n",
         "gain.out = -9"},
        {compositions / "combiner_roles.json", "recorded_combined.csv",
         "step,value\n1,7\n2,7\n", "combiner.combined = 7"},
        {compositions / "bound_arguments.json", "recorded_bound.csv",
         "step,value\n1,105\n2,105\n", "gain.out = 105"},
        {compositions / "interval-transfer.json", "out/interval-receipts.csv",
         "instance_id,transfer_id,epoch_julian_day,start_s,end_s,"
         "offered_water_m3,accepted_water_m3,rejected_water_m3,scale,"
         "accepted_enthalpy_j,species.NH4_N_kg-N,species.NO3_N_kg-N\n"
         "\"conservative-receiver\",\"controlled-source:1\",2440587.5,0,60,2,"
         "2,0,1,100,0.20000000000000001,0.10000000000000001\n"
         "\"conservative-receiver\",\"controlled-source:2\",2440587.5,60,90,3,"
         "3,0,1,150,0.29999999999999999,0.14999999999999999\n",
         "conservative-receiver.cumulative.water_m3 = 5"},
        {compositions / "demo_b_meshgen.json", "", "",
         "meshgen.mesh: 28 nodes, 26 faces"},
    };

    int failures = 0;
    {
        Plugins plugins;
        for (const Run &run : runs)
        {
            std::cout << run.document.filename().string() << '\n';
            std::string outputs, message;
            if (!runComposition(plugins, run.document, outputs, message))
            {
                std::cout << "  run failed: " << message << '\n';
                ++failures;
                continue;
            }
            if (outputs.find(run.output) == std::string::npos)
            {
                std::cout << "  ^ no output line \"" << run.output << "\"\n";
                ++failures;
            }
            if (!run.file.empty())
            {
                const std::string text = fileText(run.file);
                std::cout << "  " << run.file << ":\n" << text;
                if (text != run.contents)
                {
                    std::cout << "  ^ differs from the cookbook\n";
                    ++failures;
                }
            }
            std::cout << '\n';
        }
    }
    std::cout << (failures == 0 ? "OK\n" : "FAILED\n");
    return failures == 0 ? 0 : 1;
}
