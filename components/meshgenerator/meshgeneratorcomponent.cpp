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
 *
 * An SDK AbstractOutput that is also a
 * HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem located on
 * faces. polyhedralSurface() is null until the first successful update().
 * It owns the PolyhedralSurfaceAdapter, which owns the mesh; a new mesh
 * replaces the previous one, so a pointer taken from polyhedralSurface()
 * is valid until the next update() or the component's destruction.
 */
class MeshSurfaceOutput final
    : public AbstractOutput,
      public virtual HydroCouple::Spatial::IPolyhedralSurfaceComponentDataItem
{
  public:
    /*!
     * \brief Creates the output with no mesh.
     * \param[in] id        The output ID, `mesh`.
     * \param[in] component The owning component.
     */
    MeshSurfaceOutput(std::string_view id,
                      HydroCouple::IModelComponent *component)
      : AbstractOutput(id, {}, nullptr, component)
    {
    }

    /*!
     * \brief Replaces the served mesh and signals a value change.
     * \param[in] mesh The generated mesh, moved into the adapter.
     */
    void setMesh(IO::MeshDefinition mesh)
    {
        m_adapter = std::make_unique<SDK::Spatial::PolyhedralSurfaceAdapter>(
            SDK::Spatial::polyhedralSurfaceFromMesh(mesh), std::move(mesh));
        emitValueChanged();
    }

    //! The mesh is produced by the run, not pulled from a provider.
    void updateValues() override {}

    //! \returns An empty shape: the output carries geometry, not values.
    [[nodiscard]] std::vector<int64_t> shape() const override { return {}; }
    //! \returns DataKind::Float64, nominally; there are no values to read.
    [[nodiscard]] DataKind dataKind() const override
    {
        return DataKind::Float64;
    }
    /*!
     * \brief Refuses every read.
     * \param[out] message "The mesh output is geometry; read its
     *                     meshView()", when not null.
     * \returns false.
     */
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
    /*!
     * \brief Refuses every write.
     * \param[out] message "The mesh output is produced by the run", when
     *                     not null.
     * \returns false.
     */
    [[nodiscard]] bool setValuesFrom(const BufferDescriptor &,
                                     std::span<const int64_t>,
                                     std::span<const int64_t>,
                                     std::string *message = nullptr) override
    {
        if (message)
            *message = "The mesh output is produced by the run";
        return false;
    }

    //! On a 2-D surface a cell is a face.
    //! \returns MeshLocation::Face.
    [[nodiscard]] HydroCouple::Spatial::MeshLocation location() const override
    {
        return HydroCouple::Spatial::MeshLocation::Face;
    }
    //! \returns SpatialDataType::Scalar.
    [[nodiscard]] HydroCouple::Spatial::SpatialDataType meshDataType()
        const override
    {
        return HydroCouple::Spatial::SpatialDataType::Scalar;
    }
    //! \returns VectorBasis::Unknown: the output has no vector values.
    [[nodiscard]] HydroCouple::Spatial::VectorBasis vectorBasis()
        const override
    {
        return HydroCouple::Spatial::VectorBasis::Unknown;
    }
    /*!
     * \brief The generated surface.
     * \returns The surface, owned by this output, or null before the first
     *          successful update().
     */
    [[nodiscard]] HydroCouple::Spatial::IPolyhedralSurface *
    polyhedralSurface() const override
    {
        return m_adapter.get();
    }
    //! Geometry-only: shape() is empty, so there is no entity dimension.
    //! \returns null.
    [[nodiscard]] HydroCouple::IDimension *entityDimension() const override
    {
        return nullptr;
    }

  private:
    //! The served surface and the mesh behind it; null until the first
    //! mesh.
    std::unique_ptr<SDK::Spatial::PolyhedralSurfaceAdapter> m_adapter;
};

/*!
 * \brief Triangulates a boundary into a mesh, as a run.
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * four arguments (`boundary`, `max_edge_length`, `quad_quality`,
 * `output_path`), no inputs and one output, `mesh`, a MeshSurfaceOutput.
 * validate() checks the boundary's shape and the quality range; update()
 * does all the work in one step through Tools::Triangulator and, for a
 * quality above 0, Tools::QuadMesher, forwarding their progress stages as
 * Updating status messages, optionally writes a UGRID file, and reports
 * Done with the message "meshed". Coordinates, lengths and the quality are
 * unitless.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * The output owns the mesh. The component is driven from one thread at a
 * time, and the tools' progress callback runs on that thread.
 */
class MeshGeneratorComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.meshgenerator.instance`.
     */
    explicit MeshGeneratorComponent(std::string_view id)
      : AbstractModelComponent(id, "Mesh Generator")
    {
    }

    /*!
     * \brief Checks the boundary's size and shape and the quality range.
     * \returns Every problem found, in this order: "The boundary needs at
     *          least three vertices; it has \<n\>", "The boundary must be an
     *          N x 2 table of x, y", "quad_quality must lie in [0, 1]; 0
     *          keeps triangles". The status is Invalid when the list is not
     *          empty and Valid otherwise. The problems are returned, not
     *          queued on errors(). It does not check orientation,
     *          self-intersection or `max_edge_length`.
     */
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

    //! Moves to Updated with the message "prepared".
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

    /*!
     * \brief Generates the mesh in one step and serves it on `mesh`.
     *
     * Status messages, in order: "triangulating", the triangulator's
     * stages, the quad mesher's stages when `quad_quality` is above 0, and
     * Done with "meshed". A tool failure, or a failure writing
     * `output_path`, moves to Failed with the tool's or writer's message
     * and leaves the previous mesh in place. A relative `output_path` is
     * relative to the process's working directory.
     */
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

    /*!
     * \brief Moves through Finishing to Finished; the mesh stays readable
     *        until the component is destroyed.
     */
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    //! `boundary`, an N x 2 table of x, y; registered, never deleted.
    SDK::Argument2DDouble *boundary      = nullptr;
    //! `max_edge_length`; registered, never deleted.
    SDK::Argument1DDouble *maxEdgeLength = nullptr;
    //! `quad_quality`, in [0, 1]; registered, never deleted.
    SDK::Argument1DDouble *quadQuality   = nullptr;
    //! `output_path`, optional; registered, never deleted.
    SDK::Argument1DString *outputPath    = nullptr;
    //! The `mesh` output; registered, never deleted.
    MeshSurfaceOutput     *mesh          = nullptr;

  protected:
    /*!
     * \brief Creates the four arguments with defaults that mesh a 100 by
     *        100 square: `boundary` (0, 0), (100, 0), (100, 100), (0, 100),
     *        `max_edge_length` 0, `quad_quality` 0 and an empty optional
     *        `output_path` with the file filter "UGRID NetCDF (*.nc)".
     */
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

    /*!
     * \brief Accepts any arguments; validate() and update() read them.
     * \param[out] message Cleared.
     * \returns true.
     */
    bool initializeArguments(std::string &message) override
    {
        message.clear();
        return true;
    }

    //! The component has no inputs.
    void createInputs() override {}

    //! Creates the `mesh` output, empty until the first update().
    void createOutputs() override
    {
        mesh = new MeshSurfaceOutput("mesh", this);
        addOutput(mesh);
    }

    //! Nothing to release: argument initialization cannot fail.
    void initializeFailureCleanUp() override {}

  private:
    /*!
     * \brief Writes \p result to \p path as a UGRID NetCDF file.
     *
     * The NetCDF UGRID writer creates missing parent directories. A mesh
     * with an empty name is written as `mesh2d`; meshes from the
     * triangulator are named `triangulation`.
     * \param[in]  result  The mesh to write.
     * \param[in]  path    The file path, as given in `output_path`.
     * \param[out] message "Writing '\<path\>' failed: \<writer message\>", or,
     *                     in an SDK built without NetCDF, "output_path is
     *                     set but this SDK build has no NetCDF support;
     *                     rebuild with USE_NETCDF or clear output_path".
     * \returns false when the file was not written.
     */
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

    //! First dimension of `boundary`: one row per vertex.
    Dimension m_vertexDimension{"vertex", "Boundary vertex"};
    //! Second dimension of `boundary`: the x and y columns.
    Dimension m_xyDimension{"xy", "x, y"};
    //! The one dimension of the scalar arguments.
    Dimension m_scalarDimension{"scalar", "Single value"};
    //! Unitless quantity "Coordinate" of `boundary`.
    std::unique_ptr<Quantity> m_coordinate;
    //! Unitless quantity "Length" of `max_edge_length`.
    std::unique_ptr<Quantity> m_length;
    //! Unitless quantity "Quality" of `quad_quality`.
    std::unique_ptr<Quantity> m_quality;
    //! Unitless quantity "Path" of `output_path`.
    std::unique_ptr<Quantity> m_path;
};

/*!
 * \brief Metadata for MeshGeneratorComponent, and the factory for its
 *        instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.meshgenerator`, caption
 * "Mesh Generator", version 0.1.0, tags Provider and Mesh.
 */
class MeshGeneratorComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
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

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID
     *          `hydrocouple.components.meshgenerator.instance`, owned by
     *          the caller, who destroys it before the library is unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<MeshGeneratorComponent>(
            "hydrocouple.components.meshgenerator.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(MeshGeneratorComponentInfo)
