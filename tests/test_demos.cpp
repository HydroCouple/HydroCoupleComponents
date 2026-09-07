// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// ARGGEN P5 — the end-to-end demonstrations.
//
// Demo A (pathway a): a mesh generator, a time-series met provider, and the
// FVQual solver in ONE live composition — the mesh reaches FVQual through
// an @from binding, the met series through a second binding AND an exchange
// connection carrying a temporal-interpolation adapter, and the run is
// recorded to a manifest a results viewer can reopen.
//
// Demo B (pathway b): the mesh generator runs STANDALONE first and its mesh
// is recorded; a second composition — identical to Demo A except meshgen's
// execution block says "open" — reproduces Demo A's results from the stored
// mesh. The parity gate is the program's acceptance: Demo A ≡ Demo B, and
// the two documents differ in the execution block alone.
//
// The runner mirrors `fvqual run` (the SDK's serial_coupling recipe), with
// one addition fvqual's single-model CLI cannot make: components resolve
// through dlopen across THREE plugin libraries. Instances carry their own
// library-fixed ids, so the recorded manifests are re-keyed to the
// DOCUMENT's component ids before writing — the same contract fvqual's CLI
// satisfies by construction (its instances carry the document id).

#include <gtest/gtest.h>

#include "hydrocouplespatial.h"

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/resultsmodelcomponent.h"
#include "hydrocouplesdk/component/timesteppedworkflow.h"
#include "hydrocouplesdk/data/sdkadaptedoutputfactory.h"
#include "hydrocouplesdk/io/csvwriter.h"
#include "hydrocouplesdk/io/hdf5ugridwriter.h"
#include "hydrocouplesdk/io/iothread.h"
#include "hydrocouplesdk/io/modelinitializer.h"
#include "hydrocouplesdk/io/runmanifest.h"
#include "hydrocouplesdk/io/snapshot.h"

#include <dlfcn.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
using HydroCouple::SDK::IO::CompositionSpec;
using HydroCouple::SDK::IO::MeshDefinition;
using HydroCouple::SDK::IO::ModelInitializer;
using HydroCouple::SDK::IO::RunManifest;
using WStatus = IWorkflowComponent::WorkflowStatus;

namespace {

std::filesystem::path libraryPath(const std::filesystem::path &directory,
                                  const std::string &stem)
{
    std::filesystem::path path = directory / ("lib" + stem + ".so");
    if (!std::filesystem::exists(path))
    {
        path.replace_extension(".dylib");
    }
    return path;
}

//! Restores the working directory when a demo run leaves scope. The met
//! provider opens its source relative to the process cwd, so each demo
//! runs from its own artifact directory.
class DirectoryGuard
{
  public:
    explicit DirectoryGuard(const std::filesystem::path &directory)
        : m_previous(std::filesystem::current_path())
    {
        std::filesystem::current_path(directory);
    }
    ~DirectoryGuard() { std::filesystem::current_path(m_previous); }

  private:
    std::filesystem::path m_previous;
};

/*!
 * Opens the three plugin libraries the demos couple: the HCC mesh
 * generator and time-series provider, and FVQual's solver plugin from its
 * own repository build (FVQUAL_COMPONENTS_DIR).
 */
class DemoTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        struct Library
        {
            std::filesystem::path directory;
            std::string stem;
        };
        const std::vector<Library> libraries{
            {HCC_COMPONENTS_DIR, "hcc_meshgenerator"},
            {HCC_COMPONENTS_DIR, "hcc_timeseriesprovider"},
            {FVQUAL_COMPONENTS_DIR, "fvqual-component"},
        };

        for (const Library &library : libraries)
        {
            const std::filesystem::path path =
                libraryPath(library.directory, library.stem);
            ASSERT_TRUE(std::filesystem::exists(path))
                << library.stem << " not built at " << path;

            void *handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
            ASSERT_NE(handle, nullptr) << dlerror();
            s_handles.push_back(handle);

            auto info = reinterpret_cast<HydroCoupleComponentInfoFn>(
                dlsym(handle, HYDROCOUPLE_COMPONENT_INFO_SYMBOL));
            ASSERT_NE(info, nullptr) << dlerror();

            auto *modelInfo = dynamic_cast<IModelComponentInfo *>(info());
            ASSERT_NE(modelInfo, nullptr) << library.stem;
            s_infos[modelInfo->id()] = modelInfo;
        }

        s_demos = std::filesystem::path{HCC_ARTIFACTS_DIR} / "demos";
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

    //! Copies a committed composition (and the met CSV) into a private
    //! artifact directory, so relative paths inside it stay same-directory.
    static std::filesystem::path stage(const std::string &name,
                                       const std::filesystem::path &directory)
    {
        std::filesystem::create_directories(directory);
        const std::filesystem::path source =
            std::filesystem::path{HCC_COMPOSITIONS_DIR} / name;
        const std::filesystem::path target = directory / name;
        std::filesystem::copy_file(
            source, target, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::copy_file(
            std::filesystem::path{HCC_COMPOSITIONS_DIR} / "demo_met.csv",
            directory / "demo_met.csv",
            std::filesystem::copy_options::overwrite_existing);
        return target;
    }

    struct RunOutcome
    {
        std::filesystem::path manifestPath;
        std::map<std::string, std::string> instanceIdOf; // document → instance
    };

    /*!
     * The `fvqual run` recipe over dlopen-resolved components: realize by
     * info id, apply the document (SDK adapter factories installed), drive
     * the time-stepped workflow to Done snapshotting every capturable
     * output, and record the manifest re-keyed to document ids.
     */
    void runComposition(const std::filesystem::path &documentPath,
                        RunOutcome &outcome)
    {
        const std::filesystem::path directory = documentPath.parent_path();
        DirectoryGuard guard(directory);

        std::ifstream file(documentPath);
        ASSERT_TRUE(file.is_open()) << documentPath;
        std::stringstream text;
        text << file.rdbuf();
        std::string message;
        CompositionSpec parsed;
        ASSERT_TRUE(CompositionSpec::parse(
            nlohmann::json::parse(text.str()), parsed, message))
            << message;

        m_instances.clear();
        std::map<std::string, IModelComponent *> byId;

        for (const HydroCouple::SDK::IO::ComponentSpec &block :
             parsed.components)
        {
            const std::string infoId = block.info.componentInfoId;
            auto found = s_infos.find(infoId);
            ASSERT_NE(found, s_infos.end())
                << block.id << " names unknown info " << infoId;

            std::unique_ptr<IModelComponent> instance =
                found->second->createComponentInstance();
            ASSERT_NE(instance, nullptr);
            if (auto *abstract =
                    dynamic_cast<AbstractModelComponent *>(instance.get()))
            {
                abstract->setReferenceDirectory(directory.string());
            }
            byId[block.id] = instance.get();
            m_instances.push_back(std::move(instance));
        }

        ModelInitializer initializer(
            [&byId](const std::string &id) -> IModelComponent * {
                const auto it = byId.find(id);
                return it == byId.end() ? nullptr : it->second;
            });
        initializer.setAdapterFactoryResolver(
            [] {
                return std::vector<IAdaptedOutputFactory *>{
                    SdkAdaptedOutputFactory::instance()};
            });

        ASSERT_TRUE(initializer.initializeFromFile(documentPath, message))
            << message;
        const CompositionSpec &spec = initializer.spec();

        // ── Writers + recorder ───────────────────────────────────────────
        IO::IOThread ioThread(32);
        IO::RunRecorder recorder;
        recorder.setCaption("hcc demo runner");
        recorder.setComposition(documentPath);
        for (const IO::WriterSpec &writer : spec.writers)
        {
            ASSERT_EQ(writer.type, "csv") << "the demo runner writes csv";
            std::filesystem::path path = writer.path;
            if (path.is_relative())
            {
                path = directory / path;
            }
            auto csv = std::make_shared<IO::CSVWriter>(path);
            ioThread.addWriter(csv);
            recorder.addWriter(csv);
        }
        ASSERT_TRUE(ioThread.start());

        // ── Workflow (opened blocks resolve to their substitutes) ────────
        TimeSteppedWorkflow workflow("demo-run");
        std::vector<IModelComponent *> members;
        outcome.instanceIdOf.clear();
        for (const HydroCouple::SDK::IO::ComponentSpec &block :
             spec.components)
        {
            IModelComponent *component = initializer.component(block.id);
            ASSERT_NE(component, nullptr) << block.id;
            members.push_back(component);
            recorder.addComponent(component);
            outcome.instanceIdOf[block.id] = component->id();
            ASSERT_TRUE(workflow.addModelComponent(component, nullptr,
                                                   &message))
                << message;
        }

        workflow.initialize();
        ASSERT_EQ(workflow.status(), WStatus::Initialized);
        const std::vector<std::string> problems = workflow.validate();
        ASSERT_TRUE(problems.empty())
            << (problems.empty() ? "" : problems.front());
        workflow.prepare();
        ASSERT_EQ(workflow.status(), WStatus::Prepared);

        int step = 0;
        const int maximumSteps =
            spec.workflow.maxSteps > 0 ? spec.workflow.maxSteps : 100;
        while (workflow.status() != WStatus::Done &&
               workflow.status() != WStatus::Failed && step < maximumSteps)
        {
            workflow.update();
            ++step;

            IO::Snapshot snapshot(static_cast<double>(step));
            bool captured = false;
            for (IModelComponent *component : members)
            {
                for (IOutput *output : component->outputs())
                {
                    std::string ignored;
                    captured |= snapshot.capture(*output, ignored);
                }
            }
            if (captured)
            {
                ASSERT_TRUE(ioThread.enqueue(std::move(snapshot)));
            }
        }
        ASSERT_EQ(workflow.status(), WStatus::Done)
            << "the demo never ran to Done";

        workflow.finish();
        ioThread.finish();
        ASSERT_EQ(ioThread.errorCode(), 0);

        // ── The manifest, re-keyed to the DOCUMENT's component ids ───────
        RunManifest manifest = recorder.finish();
        rekeyToDocumentIds(manifest, outcome.instanceIdOf);
        outcome.manifestPath =
            directory / (spec.runManifest.empty() ? "run.json"
                                                  : spec.runManifest);
        ASSERT_TRUE(manifest.write(outcome.manifestPath, message)) << message;
    }

    //! The manifest catalogs by instance id(); the composition speaks in
    //! document ids. Re-key so execution mode "open" (and any results
    //! viewer walking the composition) finds its component.
    static void rekeyToDocumentIds(
        RunManifest &manifest,
        const std::map<std::string, std::string> &instanceIdOf)
    {
        for (const auto &[documentId, instanceId] : instanceIdOf)
        {
            for (auto &component : manifest.components)
            {
                if (component.id == instanceId)
                {
                    component.id = documentId;
                }
            }
            for (auto &entry : manifest.results)
            {
                if (entry.componentId == instanceId)
                {
                    entry.componentId = documentId;
                }
            }
        }
    }

    //! One recorded item, reopened the results-viewer way.
    static void readRecorded(const std::filesystem::path &manifestPath,
                             const std::string &componentId,
                             const std::string &itemId,
                             std::vector<int64_t> &shape,
                             std::vector<double> &values)
    {
        std::string message;
        auto reopened = ResultsModelComponent::open(manifestPath.string(),
                                                    componentId, message);
        ASSERT_NE(reopened, nullptr) << message;
        reopened->initialize();
        ASSERT_EQ(reopened->status(),
                  IModelComponent::ComponentStatus::Finished);

        IComponentDataItem *item = nullptr;
        for (IOutput *output : reopened->outputs())
        {
            if (output && output->id() == itemId)
            {
                item = output;
            }
        }
        ASSERT_NE(item, nullptr)
            << "no recorded " << itemId << " on " << componentId;

        shape = item->shape();
        int64_t count = 1;
        for (int64_t extent : shape)
        {
            count *= extent;
        }
        ASSERT_GT(count, 0);
        values.assign(static_cast<size_t>(count), 0.0);

        BufferDescriptor destination;
        destination.data  = values.data();
        destination.kind  = DataKind::Float64;
        destination.rank  = static_cast<int32_t>(shape.size());
        destination.shape = shape.data();
        destination.space = MemorySpace::Host;
        const std::vector<int64_t> start(shape.size(), 0);
        ASSERT_TRUE(item->getValuesInto(destination, start, shape, &message))
            << message;
    }

    //! The recorded basin temperature: values plus shape, for parity.
    static void readTemperature(const std::filesystem::path &manifestPath,
                                std::vector<int64_t> &shape,
                                std::vector<double> &values)
    {
        readRecorded(manifestPath, "basin", "temperature", shape, values);
    }

    std::vector<std::unique_ptr<IModelComponent>> m_instances;

    static std::vector<void *> s_handles;
    static std::map<std::string, IModelComponentInfo *> s_infos;
    static std::filesystem::path s_demos;
};

std::vector<void *> DemoTest::s_handles;
std::map<std::string, IModelComponentInfo *> DemoTest::s_infos;
std::filesystem::path DemoTest::s_demos;

} // namespace

TEST_F(DemoTest, DemoARunsAllThreeProvidersLiveAndItsManifestReopens)
{
    const std::filesystem::path directory = s_demos / "demo_a";
    std::filesystem::remove_all(directory);
    const std::filesystem::path document = stage("demo_a.json", directory);

    RunOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runComposition(document, outcome));

    // The results viewer's pathway: the manifest reopens with no model
    // library in sight and serves the basin's recorded temperature.
    std::vector<int64_t> shape;
    std::vector<double> values;
    ASSERT_NO_FATAL_FAILURE(
        readTemperature(outcome.manifestPath, shape, values));

    // Meteorology was enabled (only the met binding enables it) and the
    // heat budget ran: the short run cools the surface slightly.
    const double last = values.back();
    EXPECT_LT(last, 10.0) << "the met binding never enabled meteorology";
    EXPECT_GT(last, 0.0) << "the basin froze past the forcing itself";

    // And the recorded run carries the SOURCE's series, not something the
    // provider made up: every recorded air temperature is the CSV's 5 °C.
    // (Air-temperature SENSITIVITY over a 3-hour run is thousandths of a
    // degree — too thin to gate on; the served values are the sharp
    // observable.)
    std::vector<int64_t> airShape;
    std::vector<double> air;
    ASSERT_NO_FATAL_FAILURE(readRecorded(outcome.manifestPath, "met",
                                         "air_temperature", airShape, air));
    for (double sample : air)
    {
        ASSERT_EQ(sample, 5.0)
            << "the met chain served values the source never held";
    }
}

TEST_F(DemoTest, DemoBRecordsTheMeshAndOpenModeServesItBack)
{
    const std::filesystem::path directory = s_demos / "demo_b";
    std::filesystem::remove_all(directory);

    // ── Run 1: the mesh generator standalone, its mesh recorded ─────────
    const std::filesystem::path meshgenDocument =
        stage("demo_b_meshgen.json", directory);

    RunOutcome meshgenOutcome;
    {
        DirectoryGuard guard(directory);

        auto found = s_infos.find("hydrocouple.components.meshgenerator");
        ASSERT_NE(found, s_infos.end());
        std::unique_ptr<IModelComponent> meshgen =
            found->second->createComponentInstance();
        ASSERT_NE(meshgen, nullptr);

        IModelComponent *raw = meshgen.get();
        ModelInitializer initializer(
            [raw](const std::string &) -> IModelComponent * { return raw; });
        std::string message;
        ASSERT_TRUE(initializer.initializeFromFile(meshgenDocument, message))
            << message;

        TimeSteppedWorkflow workflow("meshgen-run");
        ASSERT_TRUE(workflow.addModelComponent(raw, nullptr, &message))
            << message;
        workflow.initialize();
        ASSERT_TRUE(workflow.validate().empty());
        workflow.prepare();
        int guard2 = 0;
        while (workflow.status() != WStatus::Done && ++guard2 < 8)
        {
            workflow.update();
        }
        ASSERT_EQ(workflow.status(), WStatus::Done);

        // The generated surface, copied into a MeshDefinition the UGRID
        // writer can persist.
        HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem *surfaceItem = nullptr;
        for (IOutput *output : raw->outputs())
        {
            surfaceItem = dynamic_cast<
                HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem *>(output);
            if (surfaceItem)
            {
                break;
            }
        }
        ASSERT_NE(surfaceItem, nullptr);
        const HydroCouple::Spatial::IMeshView *view =
            surfaceItem->polyhedralSurface()->meshView();
        ASSERT_NE(view, nullptr);
        ASSERT_GT(view->nodeCount(), 0);
        ASSERT_GT(view->faceCount(), 0);

        MeshDefinition mesh;
        mesh.meshName = "demo_mesh";
        const auto nodeX = view->nodeX();
        const auto nodeY = view->nodeY();
        const auto nodeZ = view->nodeZ();
        mesh.nodeX.assign(nodeX.begin(), nodeX.end());
        mesh.nodeY.assign(nodeY.begin(), nodeY.end());
        mesh.nodeZ.assign(nodeZ.begin(), nodeZ.end());
        const auto offsets = view->faceNodeOffsets();
        const auto faces   = view->faceNodes();
        mesh.faceNodeOffsets.assign(offsets.begin(), offsets.end());
        mesh.faceNodes.assign(faces.begin(), faces.end());
        const auto edges = view->edgeNodes();
        for (size_t edge = 0; edge + 1 < edges.size(); edge += 2)
        {
            mesh.edgeNodes.push_back({edges[edge], edges[edge + 1]});
        }

        IO::IOThread ioThread(8);
        auto ugrid = std::make_shared<IO::HDF5UGRIDWriter>(
            directory / "meshgen_mesh.h5", mesh);
        // The FIELD is named "mesh" so the reopened item answers to the
        // same output id the @from binding names on the live generator.
        ugrid->addField("mesh", IO::MeshLocation::Node, "m", "node_z");
        ioThread.addWriter(ugrid);

        IO::RunRecorder recorder;
        recorder.setCaption("hcc demo meshgen");
        recorder.setComposition(meshgenDocument);
        recorder.addWriter(ugrid);
        recorder.addComponent(raw);
        ASSERT_TRUE(ioThread.start());

        // One node field carries the artifact (a 2D generator serves no z,
        // so the values are zeros over the mesh's nodes — the GEOMETRY is
        // what the second composition reads back).
        std::vector<double> nodeField(mesh.nodeX.size(), 0.0);
        if (mesh.nodeZ.size() == mesh.nodeX.size())
        {
            nodeField = mesh.nodeZ;
        }
        IO::Snapshot snapshot(0.0);
        IO::FieldSlice slice;
        slice.itemId = "mesh";
        slice.kind   = DataKind::Float64;
        slice.shape  = {static_cast<int64_t>(nodeField.size())};
        slice.data.resize(nodeField.size() * sizeof(double));
        std::memcpy(slice.data.data(), nodeField.data(), slice.data.size());
        snapshot.addField(std::move(slice));
        ASSERT_TRUE(ioThread.enqueue(std::move(snapshot)));

        raw->finish();
        ioThread.finish();
        ASSERT_EQ(ioThread.errorCode(), 0);

        RunManifest manifest = recorder.finish();
        rekeyToDocumentIds(manifest, {{"meshgen", raw->id()}});
        meshgenOutcome.manifestPath = directory / "meshgen_run.json";
        ASSERT_TRUE(manifest.write(meshgenOutcome.manifestPath, message))
            << message;

        // The stored mesh reopens as a real polyhedral surface — this is
        // the item the second composition's @from binding will read.
        auto reopened = ResultsModelComponent::open(
            meshgenOutcome.manifestPath.string(), "meshgen", message);
        ASSERT_NE(reopened, nullptr) << message;
        reopened->initialize();
        ASSERT_EQ(reopened->status(),
                  IModelComponent::ComponentStatus::Finished);
        HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem *stored = nullptr;
        for (IOutput *output : reopened->outputs())
        {
            if (output && output->id() == "mesh")
            {
                stored = dynamic_cast<
                    HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem *>(output);
            }
        }
        ASSERT_NE(stored, nullptr)
            << "the recorded mesh did not reopen as a surface item";
        ASSERT_NE(stored->polyhedralSurface(), nullptr);
        EXPECT_EQ(stored->polyhedralSurface()->meshView()->faceCount(),
                  view->faceCount());
    }

    // ── Run 2: Demo A's composition with meshgen opened, not run ────────
    const std::filesystem::path openDocument =
        stage("demo_b_open.json", directory);

    RunOutcome outcome;
    ASSERT_NO_FATAL_FAILURE(runComposition(openDocument, outcome));

    std::vector<int64_t> shape;
    std::vector<double> values;
    ASSERT_NO_FATAL_FAILURE(
        readTemperature(outcome.manifestPath, shape, values));
    EXPECT_LT(values.back(), 10.0);
}

TEST_F(DemoTest, DemoAAndDemoBAgreeAndDifferOnlyInTheExecutionBlock)
{
    // The documents: identical except meshgen's execution block. Parsed
    // comparison, so formatting cannot hide a real divergence.
    const auto load = [](const std::string &name) {
        std::ifstream file(std::filesystem::path{HCC_COMPOSITIONS_DIR} /
                           name);
        EXPECT_TRUE(file.is_open()) << name;
        std::stringstream text;
        text << file.rdbuf();
        return nlohmann::json::parse(text.str());
    };

    nlohmann::json demoA = load("demo_a.json");
    nlohmann::json demoB = load("demo_b_open.json");
    ASSERT_TRUE(demoB["components"][0].contains("execution"))
        << "demo B's meshgen must declare execution mode open";
    EXPECT_EQ(demoB["components"][0]["execution"]["mode"], "open");
    demoB["components"][0].erase("execution");
    EXPECT_EQ(demoA, demoB)
        << "the two demo documents diverge beyond the execution block";

    // The results: byte-identical temperature history. Both runs must have
    // happened (this gate orders after them via --gtest_also_run_disabled
    // ordering being unnecessary: gtest runs fixtures in declaration
    // order within one binary).
    const std::filesystem::path manifestA =
        s_demos / "demo_a" / "run.json";
    const std::filesystem::path manifestB =
        s_demos / "demo_b" / "run.json";
    ASSERT_TRUE(std::filesystem::exists(manifestA)) << "run Demo A first";
    ASSERT_TRUE(std::filesystem::exists(manifestB)) << "run Demo B first";

    std::vector<int64_t> shapeA, shapeB;
    std::vector<double> valuesA, valuesB;
    ASSERT_NO_FATAL_FAILURE(readTemperature(manifestA, shapeA, valuesA));
    ASSERT_NO_FATAL_FAILURE(readTemperature(manifestB, shapeB, valuesB));

    EXPECT_EQ(shapeA, shapeB)
        << "the opened mesh gave the basin a different geometry";
    ASSERT_EQ(valuesA.size(), valuesB.size());
    EXPECT_EQ(std::memcmp(valuesA.data(), valuesB.data(),
                          valuesA.size() * sizeof(double)),
              0)
        << "Demo A and Demo B computed different temperatures";
}
