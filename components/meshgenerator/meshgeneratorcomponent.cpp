// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   meshgeneratorcomponent.cpp
 * \brief  MeshGeneratorComponent — the SDK's triangulator as a component.
 *
 * The first real provider: a domain goes in as arguments, the run
 * triangulates it (optionally merging to quad-dominant), and the mesh
 * comes out as a polyhedral-surface output that a consumer's mesh argument
 * adopts through an \c \@from binding — or, when \c output_path is set, as
 * a UGRID file for the standalone pathway.
 *
 * Generation is strictly a delegation to \c Tools::Triangulator and
 * \c Tools::QuadMesher: a component that reordered, welded, or nudged
 * anything would produce a mesh nobody else in the ecosystem can
 * reproduce, and the tests compare node for node against direct calls to
 * prove it does not.
 *
 * All the work happens in update(), never in initialize() — a composition
 * host initializes components merely to see their ports, and must be able
 * to do so without paying for a triangulation.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/abstractoutput.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/argument2d.h"
#include "hydrocouplesdk/spatial/meshadapters.h"
#include "hydrocouplesdk/tools/quadmesher.h"
#include "hydrocouplesdk/tools/triangulator.h"

#ifdef USE_NETCDF
#include "hydrocouplesdk/io/netcdfugridwriter.h"
#endif

#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief The generated mesh as a polyhedral-surface data item.
 *
 * Geometry-only: the surface and its mesh view are the payload, and the
 * hyperslab plane says so instead of inventing per-face values nobody
 * asked for. A consumer's PolyhedralSurfaceArgument reads the meshView().
 */
class MeshSurfaceOutput final
    : public AbstractOutput,
      public virtual HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem
{
  public:
    MeshSurfaceOutput(std::string_view id,
                      HydroCouple::IModelComponent *component)
      : AbstractOutput(id, {}, nullptr, component)
    {
    }

    void setMesh(IO::MeshDefinition mesh)
    {
        m_adapter = std::make_unique<SDK::Spatial::PolyhedralSurfaceAdapter>(
            SDK::Spatial::polyhedralSurfaceFromMesh(mesh), std::move(mesh));
        emitValueChanged();
    }

    //! The mesh is produced by the run, not pulled from a provider.
    void updateValues() override {}

    [[nodiscard]] std::vector<int64_t> shape() const override { return {}; }
    [[nodiscard]] DataKind dataKind() const override
    {
        return DataKind::Float64;
    }
    [[nodiscard]] bool getValuesInto(const BufferDescriptor &,
                                     std::span<const int64_t>,
                                     std::span<const int64_t>,
                                     std::string *message = nullptr)
        const override
    {
        if (message)
            *message = "The mesh output is geometry; read its meshView()";
        return false;
    }
    [[nodiscard]] bool setValuesFrom(const BufferDescriptor &,
                                     std::span<const int64_t>,
                                     std::span<const int64_t>,
                                     std::string *message = nullptr) override
    {
        if (message)
            *message = "The mesh output is produced by the run";
        return false;
    }

    [[nodiscard]] HydroCouple::Spatial::MeshDataObjectType
    meshDataObjectType() const override
    {
        return HydroCouple::Spatial::MeshDataObjectType::Cell;
    }
    [[nodiscard]] HydroCouple::Spatial::SpatialDataType meshDataType()
        const override
    {
        return HydroCouple::Spatial::SpatialDataType::Scalar;
    }
    [[nodiscard]] HydroCouple::Spatial::IPolyhedralSurface *
    polyhedralSurface() const override
    {
        return m_adapter.get();
    }
    [[nodiscard]] HydroCouple::IDimension *patchDimension() const override
    {
        return nullptr;
    }
    [[nodiscard]] HydroCouple::IDimension *edgeDimension() const override
    {
        return nullptr;
    }
    [[nodiscard]] HydroCouple::IDimension *vertexDimension() const override
    {
        return nullptr;
    }

  private:
    std::unique_ptr<SDK::Spatial::PolyhedralSurfaceAdapter> m_adapter;
};

/*!
 * \brief Triangulates a boundary into a mesh, as a run.
 */
class MeshGeneratorComponent : public AbstractModelComponent
{
  public:
    explicit MeshGeneratorComponent(std::string_view id)
      : AbstractModelComponent(id, "Mesh Generator")
    {
    }

    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        std::vector<std::string> problems;
        if (boundary->iLength() < 3)
            problems.push_back(
                "The boundary needs at least three vertices; it has " +
                std::to_string(boundary->iLength()));
        if (boundary->jLength() != 2)
            problems.push_back("The boundary must be an N x 2 table of x, y");
        const double quality = (*quadQuality)[0];
        if (quality < 0.0 || quality > 1.0)
            problems.push_back(
                "quad_quality must lie in [0, 1]; 0 keeps triangles");
        setStatus(problems.empty() ? ComponentStatus::Valid
                                   : ComponentStatus::Invalid);
        return problems;
    }

    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating, "triangulating");

        Tools::TriangulationInput input;
        const int vertexCount = static_cast<int>(boundary->iLength());
        input.boundary.reserve(static_cast<size_t>(vertexCount));
        for (int vertex = 0; vertex < vertexCount; ++vertex)
            input.boundary.emplace_back((*boundary)(vertex, 0),
                                        (*boundary)(vertex, 1));
        input.maxEdgeLength = (*maxEdgeLength)[0];

        // Stage text rides the status events, so a host watching the
        // component sees what the run is doing without new plumbing.
        const Tools::MeshProgress progress =
            [this](double, const char *stage) {
                setStatus(ComponentStatus::Updating, stage);
                return true;
            };

        IO::MeshDefinition triangles;
        std::string message;
        if (!Tools::Triangulator::triangulate(input, triangles, message,
                                              progress))
        {
            setStatus(ComponentStatus::Failed, message);
            return;
        }

        IO::MeshDefinition result;
        const double quality = (*quadQuality)[0];
        if (quality > 0.0)
        {
            if (!Tools::QuadMesher::triToQuadDominant(triangles, quality,
                                                      result, message,
                                                      progress))
            {
                setStatus(ComponentStatus::Failed, message);
                return;
            }
        }
        else
        {
            result = std::move(triangles);
        }

        const std::string path = (*outputPath)[0];
        if (!path.empty() && !writeUgrid(result, path, message))
        {
            setStatus(ComponentStatus::Failed, message);
            return;
        }

        mesh->setMesh(std::move(result));
        setStatus(ComponentStatus::Done, "meshed");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    SDK::Argument2DDouble *boundary      = nullptr;
    SDK::Argument1DDouble *maxEdgeLength = nullptr;
    SDK::Argument1DDouble *quadQuality   = nullptr;
    SDK::Argument1DString *outputPath    = nullptr;
    MeshSurfaceOutput     *mesh          = nullptr;

  protected:
    void createArguments() override
    {
        // The defaults mesh a 100 x 100 square: a composition host
        // initializes components just to see their ports, and an
        // unconfigured generator must still be a valid one.
        m_coordinate.reset(Quantity::unitLess("Coordinate"));
        boundary = new SDK::Argument2DDouble("boundary", &m_vertexDimension,
                                             &m_xyDimension, 4, 2,
                                             m_coordinate.get(), this);
        (*boundary)(0, 0) = 0.0;   (*boundary)(0, 1) = 0.0;
        (*boundary)(1, 0) = 100.0; (*boundary)(1, 1) = 0.0;
        (*boundary)(2, 0) = 100.0; (*boundary)(2, 1) = 100.0;
        (*boundary)(3, 0) = 0.0;   (*boundary)(3, 1) = 100.0;
        addArgument(boundary);

        m_length.reset(Quantity::unitLess("Length"));
        maxEdgeLength = new SDK::Argument1DDouble(
            "max_edge_length", &m_scalarDimension, 1, m_length.get(), this);
        (*maxEdgeLength)[0] = 0.0;
        addArgument(maxEdgeLength);

        m_quality.reset(Quantity::unitLess("Quality"));
        quadQuality = new SDK::Argument1DDouble(
            "quad_quality", &m_scalarDimension, 1, m_quality.get(), this);
        (*quadQuality)[0] = 0.0;
        addArgument(quadQuality);

        m_path.reset(Quantity::unitLess("Path"));
        outputPath = new SDK::Argument1DString(
            "output_path", &m_scalarDimension, 1, m_path.get(), this);
        (*outputPath)[0] = std::string();
        outputPath->addFileFilter("UGRID NetCDF (*.nc)");
        outputPath->setIsOptional(true);
        addArgument(outputPath);
    }

    bool initializeArguments(std::string &message) override
    {
        message.clear();
        return true;
    }

    void createInputs() override {}

    void createOutputs() override
    {
        mesh = new MeshSurfaceOutput("mesh", this);
        addOutput(mesh);
    }

    void initializeFailureCleanUp() override {}

  private:
    [[nodiscard]] bool writeUgrid(const IO::MeshDefinition &result,
                                  const std::string &path,
                                  std::string &message)
    {
#ifdef USE_NETCDF
        IO::MeshDefinition named = result;
        if (named.meshName.empty())
            named.meshName = "mesh2d";
        IO::NetCDFUGRIDWriter writer(path, std::move(named));
        if (writer.initialize() != 0 || writer.finalize() != 0)
        {
            message = "Writing '" + path + "' failed: " +
                      writer.errorMessage();
            return false;
        }
        return true;
#else
        message = "output_path is set but this SDK build has no NetCDF "
                  "support; rebuild with USE_NETCDF or clear output_path";
        return false;
#endif
    }

    Dimension m_vertexDimension{"vertex", "Boundary vertex"};
    Dimension m_xyDimension{"xy", "x, y"};
    Dimension m_scalarDimension{"scalar", "Single value"};
    std::unique_ptr<Quantity> m_coordinate;
    std::unique_ptr<Quantity> m_length;
    std::unique_ptr<Quantity> m_quality;
    std::unique_ptr<Quantity> m_path;
};

/*!
 * \brief Metadata for MeshGeneratorComponent.
 */
class MeshGeneratorComponentInfo : public AbstractModelComponentInfo
{
  public:
    MeshGeneratorComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.meshgenerator")
    {
        setCaption("Mesh Generator");
        setDescription(
            "Triangulates a boundary polygon (optionally quad-dominant) "
            "and provides the mesh to other components, live through an "
            "@from binding or as a UGRID file.");
        setDeveloper("HydroCouple");
        setVersion("0.1.0");
        setTags({"Provider", "Mesh"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<MeshGeneratorComponent>(
            "hydrocouple.components.meshgenerator.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(MeshGeneratorComponentInfo)
