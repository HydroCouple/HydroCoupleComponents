// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin
//
// Gates for MeshGeneratorComponent, driven through dlopen exactly as a
// host drives it. The suite's spine is the delegation gate: a mesh built
// through the component and one built by calling the SDK tools directly on
// the same domain are compared node for node and face for face — a
// component that reordered, welded, or nudged anything would still count
// right, and counts are not the claim.

#include <gtest/gtest.h>

#include "hydrocouplesdk/componentabi.h"
#include "hydrocouplesdk/io/modelinitializer.h"
#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/spatial/polyhedralsurfaceargument.h"
#include "hydrocouplesdk/tools/quadmesher.h"
#include "hydrocouplesdk/tools/triangulator.h"

#include <dlfcn.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
using HydroCouple::SDK::IO::MeshDefinition;

namespace {

//! An asymmetric quadrilateral, so triangulations cannot pass by symmetry.
const std::vector<std::array<double, 2>> kBoundary = {
    {0.0, 0.0}, {40.0, 0.0}, {45.0, 28.0}, {5.0, 33.0}};

Tools::TriangulationInput directInput(double maxEdgeLength)
{
    Tools::TriangulationInput input;
    for (const auto &vertex : kBoundary)
        input.boundary.emplace_back(vertex[0], vertex[1]);
    input.maxEdgeLength = maxEdgeLength;
    return input;
}

/*!
 * The plugin, loaded once for the whole suite the way a host keeps a
 * library loaded for its registry's lifetime.
 */
class MeshGeneratorTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        std::filesystem::path path =
            std::filesystem::path(HCC_COMPONENTS_DIR) /
            "libhcc_meshgenerator.so";
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

    //! Runs one instance over kBoundary and returns it, Finished.
    static std::unique_ptr<IModelComponent> runGenerator(
        double maxEdgeLength, double quadQuality,
        const std::string &outputPath = std::string())
    {
        using Status = IModelComponent::ComponentStatus;
        std::unique_ptr<IModelComponent> component =
            s_info->createComponentInstance();
        if (!component)
            return nullptr;
        component->initialize();
        if (component->status() != Status::Initialized)
            return nullptr;

        nlohmann::json rows = nlohmann::json::array();
        for (const auto &vertex : kBoundary)
            rows.push_back({vertex[0], vertex[1]});
        std::string message;
        auto setArgument = [&](const std::string &id,
                               const nlohmann::json &payload) {
            for (IArgument *argument : component->arguments())
            {
                if (argument->id() != id)
                    continue;
                return argument->initialize(payload.dump(),
                                            IArgument::ArgumentInputType::JSON,
                                            message);
            }
            return false;
        };
        if (!setArgument("boundary", {{"values", rows}}) ||
            !setArgument("max_edge_length",
                         {{"values", {maxEdgeLength}}}) ||
            !setArgument("quad_quality", {{"values", {quadQuality}}}))
        {
            ADD_FAILURE() << message;
            return nullptr;
        }
        if (!outputPath.empty() &&
            !setArgument("output_path", {{"values", {outputPath}}}))
        {
            ADD_FAILURE() << message;
            return nullptr;
        }

        if (!component->validate().empty())
            return nullptr;
        component->prepare();
        while (component->status() != Status::Done &&
               component->status() != Status::Failed)
            component->update();
        component->finish();
        if (component->status() != Status::Finished)
            return nullptr;
        return component;
    }

    //! The produced mesh, copied out through a PolyhedralSurfaceArgument —
    //! the exact adoption path a consuming component uses.
    static MeshDefinition producedMesh(IModelComponent &component)
    {
        SDK::Spatial::PolyhedralSurfaceArgument probe("probe", nullptr);
        std::string message;
        EXPECT_TRUE(probe.initialize(
            static_cast<const IComponentDataItem &>(*component.outputs()[0]),
            message))
            << message;
        return probe.mesh();
    }

    static void *s_handle;
    static IModelComponentInfo *s_info;
};

void *MeshGeneratorTest::s_handle = nullptr;
IModelComponentInfo *MeshGeneratorTest::s_info = nullptr;

} // namespace

TEST_F(MeshGeneratorTest, TheGeneratedMeshIsTheToolsOwnNodeForNode)
{
    auto component = runGenerator(8.0, 0.0);
    ASSERT_NE(component, nullptr);
    const MeshDefinition viaComponent = producedMesh(*component);

    MeshDefinition direct;
    std::string message;
    ASSERT_TRUE(Tools::Triangulator::triangulate(directInput(8.0), direct,
                                                 message))
        << message;

    ASSERT_EQ(viaComponent.nodeCount(), direct.nodeCount());
    EXPECT_EQ(viaComponent.nodeX, direct.nodeX);
    EXPECT_EQ(viaComponent.nodeY, direct.nodeY);
    EXPECT_EQ(viaComponent.faceNodeOffsets, direct.faceNodeOffsets);
    EXPECT_EQ(viaComponent.faceNodes, direct.faceNodes);
    EXPECT_GT(direct.faceCount(), 2) << "an untriangulated square would "
                                        "make this gate near-vacuous";
}

TEST_F(MeshGeneratorTest, QuadMergingFollowsTheQualityArgument)
{
    auto component = runGenerator(8.0, 0.6);
    ASSERT_NE(component, nullptr);
    const MeshDefinition viaComponent = producedMesh(*component);

    MeshDefinition triangles, direct;
    std::string message;
    ASSERT_TRUE(Tools::Triangulator::triangulate(directInput(8.0), triangles,
                                                 message))
        << message;
    ASSERT_TRUE(Tools::QuadMesher::triToQuadDominant(triangles, 0.6, direct,
                                                     message))
        << message;

    EXPECT_EQ(viaComponent.faceNodeOffsets, direct.faceNodeOffsets);
    EXPECT_EQ(viaComponent.faceNodes, direct.faceNodes);
    EXPECT_LT(direct.faceCount(), triangles.faceCount())
        << "no pair merged: the quality gate never engaged";
}

TEST_F(MeshGeneratorTest, DefaultsProduceAValidMeshOutOfTheBox)
{
    // The D4 contract: a host initializes components just to see ports,
    // and an unconfigured generator must be valid and runnable as-is.
    using Status = IModelComponent::ComponentStatus;
    std::unique_ptr<IModelComponent> component =
        s_info->createComponentInstance();
    component->initialize();
    ASSERT_EQ(component->status(), Status::Initialized);
    EXPECT_TRUE(component->validate().empty());
    component->prepare();
    while (component->status() != Status::Done &&
           component->status() != Status::Failed)
        component->update();
    component->finish();
    ASSERT_EQ(component->status(), Status::Finished);
    EXPECT_GT(producedMesh(*component).faceCount(), 0);
}

TEST_F(MeshGeneratorTest, ATwoVertexBoundaryFailsValidationByName)
{
    std::unique_ptr<IModelComponent> component =
        s_info->createComponentInstance();
    component->initialize();
    std::string message;
    for (IArgument *argument : component->arguments())
    {
        if (argument->id() == "boundary")
            ASSERT_TRUE(argument->initialize(
                R"({"values": [[0, 0], [10, 0]]})",
                IArgument::ArgumentInputType::JSON, message))
                << message;
    }
    const std::vector<std::string> problems = component->validate();
    ASSERT_FALSE(problems.empty());
    EXPECT_NE(problems[0].find("three vertices"), std::string::npos)
        << problems[0];
}

TEST_F(MeshGeneratorTest, AnFromBindingHandsTheMeshToAConsumersArgument)
{
    // The pipeline proof: a composition binds a consumer's mesh argument
    // to the generator's output, and the initializer runs the generator
    // and adopts the mesh — no file anywhere.
    class Consumer : public AbstractModelComponent
    {
      public:
        explicit Consumer(std::string_view id) : AbstractModelComponent(id)
        {}
        std::vector<std::string> validate() override { return {}; }
        void prepare() override {}
        void update(const std::vector<IOutput *> & = {}) override {}
        void finish() override {}
        SDK::Spatial::PolyhedralSurfaceArgument *meshArgument = nullptr;

      protected:
        void createArguments() override
        {
            meshArgument =
                new SDK::Spatial::PolyhedralSurfaceArgument("mesh", this);
            addArgument(meshArgument);
        }
        bool initializeArguments(std::string &message) override
        {
            message.clear();
            return true;
        }
        void createInputs() override {}
        void createOutputs() override {}
        void initializeFailureCleanUp() override {}
    };

    Consumer consumer("solver");
    std::unique_ptr<IModelComponent> generator =
        s_info->createComponentInstance();
    IO::ModelInitializer initializer(
        [&](const std::string &id) -> IModelComponent * {
            if (id == "solver")  return &consumer;
            if (id == "meshgen") return generator.get();
            return nullptr;
        });

    std::string message;
    ASSERT_TRUE(initializer.initializeFromJsonText(R"({
        "components": [
            { "id": "solver",
              "arguments": { "mesh": { "@from": {
                  "component": "meshgen", "output": "mesh" } } } },
            { "id": "meshgen" }
        ]
    })", message)) << message;

    ASSERT_NE(consumer.meshArgument, nullptr);
    EXPECT_TRUE(consumer.meshArgument->hasMesh());
    EXPECT_GT(consumer.meshArgument->mesh().faceCount(), 0);
}

#ifdef USE_NETCDF
#include "hydrocouplesdk/io/ugridreader.h"

TEST_F(MeshGeneratorTest, TheWrittenUgridFileHoldsTheProducedMesh)
{
    const std::filesystem::path path =
        std::filesystem::path(HCC_COMPONENTS_DIR) / "meshgen_out.nc";
    auto component = runGenerator(8.0, 0.0, path.string());
    ASSERT_NE(component, nullptr);
    const MeshDefinition produced = producedMesh(*component);

    MeshDefinition read;
    std::string message;
    ASSERT_TRUE(IO::readUGRIDMesh(path, std::string(), read, message))
        << message;
    EXPECT_EQ(read.nodeX, produced.nodeX);
    EXPECT_EQ(read.faceNodes, produced.faceNodes);
}
#endif
