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
 */
class GainTransformerComponent : public AbstractModelComponent
{
  public:
    explicit GainTransformerComponent(std::string_view id)
      : AbstractModelComponent(id, "Gain Transformer")
    {
    }

    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

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

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    Input1DDouble *in = nullptr;
    Output1DDouble *out = nullptr;

  protected:
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

    bool initializeArguments(std::string &message) override
    {
        m_gain = (*m_gainArgument)[0];
        m_bias = (*m_biasArgument)[0];
        message.clear();
        return true;
    }

    void createInputs() override
    {
        m_inQuantity.reset(Quantity::unitLess("Signal"));
        in = new Input1DDouble("in", &m_scalar, 1, m_inQuantity.get(), this);
        addInput(in);
    }

    void createOutputs() override
    {
        m_outQuantity.reset(Quantity::unitLess("Transformed"));
        out = new Output1DDouble("out", &m_scalar, 1, m_outQuantity.get(),
                                 this);
        addOutput(out);
    }

    void initializeFailureCleanUp() override {}

  private:
    Dimension m_scalar{"scalar"};
    std::unique_ptr<Quantity> m_argQuantity;
    std::unique_ptr<Quantity> m_inQuantity;
    std::unique_ptr<Quantity> m_outQuantity;

    Argument1DDouble *m_gainArgument = nullptr;
    Argument1DDouble *m_biasArgument = nullptr;

    double m_gain = 1.0;
    double m_bias = 0.0;
};

class GainTransformerComponentInfo : public AbstractModelComponentInfo
{
  public:
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

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<GainTransformerComponent>(
            "hydrocouple.components.gain.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(GainTransformerComponentInfo)
