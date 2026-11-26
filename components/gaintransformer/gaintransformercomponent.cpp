// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   gaintransformercomponent.cpp
 * \brief  y = gain * x + bias — the testing family's transformer.
 *
 * The first shipped component in the ecosystem that DECLARES AN INPUT:
 * every prior plugin was a pure provider, so drag-to-connect on the canvas
 * had nothing real to land on outside test fixtures.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief One input, one output, a linear map between them each step.
 *
 * Reports Done as soon as every connected provider component is Done or
 * Finished — a transformer follows its upstream's clock; unconnected, it
 * is done after a single step.
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * two Float64 arguments, `gain` (default 1) and `bias` (default 0), one
 * input `in` and one output `out`, both unitless rank-1 Float64 items of
 * length 1. Each update() pulls the provider's first value into `in` and
 * writes `out = gain * in + bias`. Unconnected, `in` keeps its last value
 * (0 after initialization).
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * It is driven from one thread at a time.
 */
class GainTransformerComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.gain.instance`.
     */
    explicit GainTransformerComponent(std::string_view id)
      : AbstractModelComponent(id, "Gain Transformer")
    {
    }

    /*!
     * \brief Reports Valid: any gain and bias are accepted, and a missing
     *        provider is allowed.
     * \returns An empty list.
     */
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    /*!
     * \brief Moves to Updated with the message "prepared".
     */
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

    /*!
     * \brief Pulls `in` from its provider and writes `gain * in + bias`
     *        to `out`.
     *
     * Reports Done with the message "stepped" when the provider's
     * component is Done or Finished, or when `in` has no provider or the
     * provider has no component; otherwise Updated with the same message.
     */
    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating);

        applyInputValues();
        (*out)[0] = m_gain * (*in)[0] + m_bias;

        bool upstreamDone = true;

        if (in->provider() && in->provider()->modelComponent())
        {
            const ComponentStatus status =
                in->provider()->modelComponent()->status();
            upstreamDone = status == ComponentStatus::Done ||
                           status == ComponentStatus::Finished;
        }

        setStatus(upstreamDone ? ComponentStatus::Done
                               : ComponentStatus::Updated,
                  "stepped");
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

    //! The `in` input; registered with the base class, which does not
    //! delete it; null before the first initialize().
    Input1DDouble *in = nullptr;
    //! The `out` output; registered with the base class, which does not
    //! delete it; null before the first initialize().
    Output1DDouble *out = nullptr;

  protected:
    //! Creates `gain` (default 1) and `bias` (default 0).
    void createArguments() override
    {
        m_argQuantity.reset(Quantity::unitLess("Gain argument"));

        m_gainArgument = new Argument1DDouble("gain", &m_scalar, 1,
                                              m_argQuantity.get(), this);
        (*m_gainArgument)[0] = 1.0;
        addArgument(m_gainArgument);

        m_biasArgument = new Argument1DDouble("bias", &m_scalar, 1,
                                              m_argQuantity.get(), this);
        (*m_biasArgument)[0] = 0.0;
        addArgument(m_biasArgument);
    }

    /*!
     * \brief Latches the first value of `gain` and of `bias`.
     * \param[out] message Cleared.
     * \returns true. An argument holding no value makes the element access
     *          throw std::out_of_range instead.
     */
    bool initializeArguments(std::string &message) override
    {
        m_gain = (*m_gainArgument)[0];
        m_bias = (*m_biasArgument)[0];
        message.clear();
        return true;
    }

    /*!
     * \brief Creates `in`: rank 1, length 1, Float64, unitless "Signal".
     *
     * It accepts any rank-1 Float64 provider and reads only its first
     * value.
     */
    void createInputs() override
    {
        m_inQuantity.reset(Quantity::unitLess("Signal"));
        in = new Input1DDouble("in", &m_scalar, 1, m_inQuantity.get(), this);
        addInput(in);
    }

    //! Creates `out`: rank 1, length 1, Float64, unitless "Transformed".
    void createOutputs() override
    {
        m_outQuantity.reset(Quantity::unitLess("Transformed"));
        out = new Output1DDouble("out", &m_scalar, 1, m_outQuantity.get(),
                                 this);
        addOutput(out);
    }

    //! Nothing to release: argument initialization cannot fail.
    void initializeFailureCleanUp() override {}

  private:
    //! The one dimension of every argument and exchange item.
    Dimension m_scalar{"scalar"};
    //! Unitless quantity of the two arguments.
    std::unique_ptr<Quantity> m_argQuantity;
    //! Unitless quantity "Signal" of `in`.
    std::unique_ptr<Quantity> m_inQuantity;
    //! Unitless quantity "Transformed" of `out`.
    std::unique_ptr<Quantity> m_outQuantity;

    //! `gain` argument; registered, never deleted.
    Argument1DDouble *m_gainArgument = nullptr;
    //! `bias` argument; registered, never deleted.
    Argument1DDouble *m_biasArgument = nullptr;

    //! Latched gain.
    double m_gain = 1.0;
    //! Latched bias.
    double m_bias = 0.0;
};

/*!
 * \brief Metadata for GainTransformerComponent, and the factory for its
 *        instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.gain`, caption
 * "Gain Transformer", version 1.0.0, tags Test and Transformer.
 */
class GainTransformerComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
    GainTransformerComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.gain")
    {
        setCaption("Gain Transformer");
        setDescription(
            "Applies y = gain * x + bias to its input each step. The testing "
            "family's transformer, and the ecosystem's first shipped "
            "component with an input.");
        setDeveloper("HydroCouple");
        setVersion("1.0.0");
        setTags({"Test", "Transformer"});
    }

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID `hydrocouple.components.gain.instance`,
     *          owned by the caller, who destroys it before the library is
     *          unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<GainTransformerComponent>(
            "hydrocouple.components.gain.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(GainTransformerComponentInfo)
