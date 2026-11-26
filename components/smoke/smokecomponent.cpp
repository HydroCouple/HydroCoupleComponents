// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   smokecomponent.cpp
 * \brief  The scaffold's proof: a minimal real component, built on the
 *         SDK's base classes and published through the stamped ABI entry
 *         points, so the repo's toolchain and loading conventions are
 *         demonstrated before any provider carries real algorithms.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief One output, produced in one step: the smallest honest provider.
 *
 * A complete HydroCouple::IModelComponent built on the SDK's
 * AbstractModelComponent: no arguments, no inputs, and one output,
 * `produced`, a rank-1 Float64 item of length 1 with the unitless quantity
 * "Answer". The first update() writes 42 to it and reports Done, so a run
 * is initialize(), validate(), prepare(), one update() and finish(). It is
 * the scaffold's proof that the loading convention works and the
 * tutorial's starting point; it is a test fixture, not a tool.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * Like every SDK component it is driven from one thread at a time.
 */
class SmokeComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.smoke.instance`, and a host may
     *               replace it with setInstanceId() before initialization.
     */
    explicit SmokeComponent(std::string_view id)
      : AbstractModelComponent(id, "Smoke Component")
    {
    }

    /*!
     * \brief Reports Valid: the component has nothing to check.
     * \returns An empty list. The status passes through Validating to
     *          Valid.
     */
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    /*!
     * \brief Moves to Updated with the status message "prepared".
     */
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

    /*!
     * \brief Writes 42 to `produced` and reports Done with the message
     *        "produced".
     *
     * Call it in the Updated state; it does not check the state, and a
     * second call writes the same value again.
     */
    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating);
        (*produced)[0] = 42.0;
        setStatus(ComponentStatus::Done, "produced");
    }

    /*!
     * \brief Moves through Finishing to Finished; there is nothing to
     *        release.
     */
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    //! The `produced` output, created by createOutputs() and registered with
    //! the base class, which does not delete it; null before the first
    //! initialize().
    Output1DDouble *produced = nullptr;

  protected:
    //! The component has no arguments.
    void createArguments() override {}

    /*!
     * \brief Accepts the (empty) argument set.
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

    /*!
     * \brief Creates the `produced` output: rank 1, length 1, Float64,
     *        unitless quantity "Answer".
     */
    void createOutputs() override
    {
        m_quantity.reset(Quantity::unitLess("Answer"));
        produced =
            new Output1DDouble("produced", &m_dimension, 1, m_quantity.get(),
                               this);
        addOutput(produced);
    }

    //! Nothing to release: argument initialization cannot fail.
    void initializeFailureCleanUp() override {}

  private:
    //! The one dimension of `produced`, "scalar".
    Dimension m_dimension{"scalar", "Single value"};
    //! The quantity of `produced`, unitless "Answer"; created with it.
    std::unique_ptr<Quantity> m_quantity;
};

/*!
 * \brief Metadata for SmokeComponent, and the factory for its instances.
 *
 * The library publishes this info through HYDROCOUPLE_DECLARE_COMPONENT as a
 * function-local static, so it lives until the library is unloaded and a
 * host must not delete it. Component info ID `hydrocouple.components.smoke`,
 * caption "Smoke Component", version 0.1.0, tags Test and Scaffold.
 */
class SmokeComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
    SmokeComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.smoke")
    {
        setCaption("Smoke Component");
        setDescription(
            "Scaffold proof component for the HydroCoupleComponents repo.");
        setDeveloper("HydroCouple");
        setVersion("0.1.0");
        setTags({"Test", "Scaffold"});
    }

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID `hydrocouple.components.smoke.instance`.
     *          The caller owns it and must destroy it before the library is
     *          unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<SmokeComponent>(
            "hydrocouple.components.smoke.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(SmokeComponentInfo)
