// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// The tutorial's host: a host in miniature. It searches the directories
// named on its command line for component libraries, loads each through the
// stamped ABI, runs moving_average.json with the installed tutorial
// component between two shipped ones, shows the component refusing a bad
// argument, destroys every instance and only then unloads the libraries.
//
//   docs_example_tutorial_moving_average <directory> [<directory> ...]

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/timesteppedworkflow.h"
#include "hydrocouplesdk/io/compositionspec.h"
#include "hydrocouplesdk/io/modelinitializer.h"

#include <dlfcn.h>

#include <algorithm>
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

namespace
{

//! [load]
//! One loaded component library: the handle and the info it published.
struct LoadedLibrary
{
    void *handle = nullptr;
    fs::path path;
    IComponentInfo *info = nullptr; // owned by the library
};

//! Loads one library the way a host must: the C stamp before any C++ call.
bool loadLibrary(const fs::path &path, LoadedLibrary &loaded,
                 std::string &message)
{
    void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        const char *error = dlerror();
        message = "cannot load: " + std::string(error ? error : "unknown");
        return false;
    }

    auto abi = reinterpret_cast<HydroCoupleComponentAbiFn>(
        dlsym(handle, HYDROCOUPLE_COMPONENT_ABI_SYMBOL));
    if (!abi)
    {
        message = "not a component library: no " +
                  std::string(HYDROCOUPLE_COMPONENT_ABI_SYMBOL);
        dlclose(handle);
        return false;
    }

    const char *stamp = abi();
    if (!stamp || std::string(stamp) != HYDROCOUPLE_COMPONENT_ABI_STAMP)
    {
        message = "refused: library stamp " +
                  std::string(stamp ? stamp : "(null)") + ", host stamp " +
                  HYDROCOUPLE_COMPONENT_ABI_STAMP;
        dlclose(handle);
        return false;
    }

    // Only now is it defined to call C++ across the boundary.
    auto infoFunction = reinterpret_cast<HydroCoupleComponentInfoFn>(
        dlsym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
    IComponentInfo *info = infoFunction ? infoFunction() : nullptr;
    if (!info)
    {
        message = "the stamp matches but there is no component info";
        dlclose(handle);
        return false;
    }

    loaded = {handle, path, info};
    message.clear();
    return true;
}
//! [load]

//! [search]
//! Loads every .so and .dylib file in each directory, in name order.
std::vector<LoadedLibrary> searchDirectories(
    const std::vector<fs::path> &directories)
{
    std::vector<LoadedLibrary> libraries;
    for (const fs::path &directory : directories)
    {
        std::vector<fs::path> candidates;
        for (const fs::directory_entry &entry :
             fs::directory_iterator(directory))
        {
            const std::string extension = entry.path().extension().string();
            if (entry.is_regular_file() &&
                (extension == ".so" || extension == ".dylib"))
                candidates.push_back(entry.path());
        }
        std::sort(candidates.begin(), candidates.end());

        for (const fs::path &candidate : candidates)
        {
            LoadedLibrary loaded;
            std::string message;
            if (loadLibrary(candidate, loaded, message))
            {
                std::cout << "loaded  " << candidate.filename().string()
                          << " -> " << loaded.info->id() << '\n';
                libraries.push_back(loaded);
            }
            else
            {
                std::cout << "skipped " << candidate.filename().string()
                          << ": " << message << '\n';
            }
        }
    }
    return libraries;
}
//! [search]

//! Finds a model component's info by its ID among the loaded libraries.
IModelComponentInfo *modelInfo(const std::vector<LoadedLibrary> &libraries,
                               const std::string &id)
{
    for (const LoadedLibrary &library : libraries)
        if (library.info->id() == id)
            return dynamic_cast<IModelComponentInfo *>(library.info);
    return nullptr;
}

//! [run]
/*!
 * Creates one instance per component block, applies the document, steps the
 * workflow to Done and finishes every component. The instances live in
 * \p instances, which the caller destroys before unloading the libraries.
 */
bool runComposition(const std::vector<LoadedLibrary> &libraries,
                    const nlohmann::json &document,
                    std::vector<std::unique_ptr<IModelComponent>> &instances,
                    std::string &message)
{
    IO::CompositionSpec spec;
    if (!IO::CompositionSpec::parse(document, spec, message))
        return false;

    std::map<std::string, IModelComponent *> byId;
    for (const IO::ComponentSpec &block : spec.components)
    {
        IModelComponentInfo *info =
            modelInfo(libraries, block.info.componentInfoId);
        if (!info)
        {
            message = "no library provides " + block.info.componentInfoId;
            return false;
        }
        std::unique_ptr<IModelComponent> instance =
            info->createComponentInstance();

        // Like Composer: the document's ID becomes the instance ID, and
        // relative file arguments resolve against the reference directory.
        auto *sdk = dynamic_cast<AbstractModelComponent *>(instance.get());
        if (sdk)
        {
            if (!sdk->setInstanceId(block.id, message))
                return false;
            sdk->setReferenceDirectory(fs::current_path().string());
        }
        byId[block.id] = instance.get();
        instances.push_back(std::move(instance));
    }

    IO::ModelInitializer initializer(
        [&byId](const std::string &id) -> IModelComponent * {
            const auto found = byId.find(id);
            return found == byId.end() ? nullptr : found->second;
        });
    if (!initializer.initialize(document, message))
        return false;

    TimeSteppedWorkflow workflow("workflow");
    for (const auto &instance : instances)
        if (!workflow.addModelComponent(instance.get(), nullptr, &message))
            return false;

    workflow.initialize();
    if (!workflow.validate().empty())
    {
        message = "the composition did not validate";
        return false;
    }
    workflow.prepare();
    for (int step = 0;
         step < 100 &&
         workflow.status() != IWorkflowComponent::WorkflowStatus::Done;
         ++step)
        workflow.update();

    for (const auto &instance : instances)
        instance->finish();
    if (workflow.status() != IWorkflowComponent::WorkflowStatus::Done)
    {
        message = "the workflow did not reach Done";
        return false;
    }
    return true;
}
//! [run]

nlohmann::json readDocument()
{
    std::ifstream file(fs::path(DOCS_EXAMPLE_SOURCE_DIR) /
                       "moving_average.json");
    return nlohmann::json::parse(file);
}

} // namespace

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: " << argv[0] << " <directory> [<directory> ...]\n";
        return 2;
    }

    std::cout << "host stamp: " << HYDROCOUPLE_COMPONENT_ABI_STAMP << "\n\n";

    std::vector<fs::path> directories(argv + 1, argv + argc);
    std::vector<LoadedLibrary> libraries = searchDirectories(directories);

    int failures = 0;
    {
        // Instances live in this scope, narrower than the libraries'.
        std::vector<std::unique_ptr<IModelComponent>> instances;
        std::string message;
        if (!runComposition(libraries, readDocument(), instances, message))
        {
            std::cout << "run failed: " << message << '\n';
            ++failures;
        }

        std::ifstream recorded("moving_average.csv");
        std::stringstream text;
        text << recorded.rdbuf();
        std::cout << "\nmoving_average.csv:\n" << text.str();
        if (text.str() != "step,value\n1,2\n2,3\n3,5\n4,3\n")
        {
            std::cout << "unexpected recording\n";
            ++failures;
        }
    }

    {
        // The same document with window 0: the component refuses it while
        // it initializes, and the initializer reports why.
        nlohmann::json document = readDocument();
        document["components"][1]["arguments"]["window"]["values"] = {0};
        std::vector<std::unique_ptr<IModelComponent>> instances;
        std::string message;
        const bool ran = runComposition(libraries, document, instances, message);
        std::cout << "\nwith window 0:\n" << message << '\n';
        if (ran || message.find("window must be at least 1, not 0") ==
                       std::string::npos)
            ++failures;
    }

    // Every instance is gone; the libraries that allocated them can close.
    for (auto library = libraries.rbegin(); library != libraries.rend();
         ++library)
        dlclose(library->handle);

    std::cout << (failures == 0 ? "\nOK\n" : "\nFAILED\n");
    return failures == 0 ? 0 : 1;
}
