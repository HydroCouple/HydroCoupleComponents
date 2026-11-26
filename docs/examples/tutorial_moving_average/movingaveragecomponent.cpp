// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   movingaveragecomponent.cpp
 * \brief  The tutorial component: the mean of the last `window` samples of
 *         its input, written from the smoke component as a starting point.
 *
 * A documentation example, built and tested by docs/examples but never
 * installed with the shipped plugins.
 */

//! [includes]
#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/unit.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <deque>
#include <memory>
#include <numeric>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;
//! [includes]

namespace
{

//! [component]
/*!
 * \brief Averages the last `window` values that arrive at its input.
 *
 * Follows its provider's clock: Done once the component that provides
 * `in` is Done or Finished, and after one update when nothing is connected.
 */
class MovingAverageComponent : public AbstractModelComponent
{
  public:
    explicit MovingAverageComponent(std::string_view id)
      : AbstractModelComponent(id, "Moving Average")
    {
    }
    //! [component]

    //! [validate]
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }
    //! [validate]

    //! [prepare]
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        m_history.clear();
        setStatus(ComponentStatus::Updated, "prepared");
    }
    //! [prepare]

    //! [update]
    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating);

        // Pull the provider's current value into `in`.
        applyInputValues();

        m_history.push_back((*m_in)[0]);
        if (static_cast<int>(m_history.size()) > m_window)
            m_history.pop_front();

        (*m_average)[0] =
            std::accumulate(m_history.begin(), m_history.end(), 0.0) /
            static_cast<double>(m_history.size());

        // A consumer keeps to its provider's clock.
        bool upstreamDone = true;
        if (m_in->provider() && m_in->provider()->modelComponent())
        {
            const ComponentStatus status =
                m_in->provider()->modelComponent()->status();
            upstreamDone = status == ComponentStatus::Done ||
                           status == ComponentStatus::Finished;
        }

        setStatus(upstreamDone ? ComponentStatus::Done
                               : ComponentStatus::Updated,
                  "averaged");
    }
    //! [update]

    //! [finish]
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }
    //! [finish]

  protected:
    //! [arguments]
    void createArguments() override
    {
        // The component owns the item; addArgument() only registers it.
        m_windowArgument = std::make_unique<Argument1DInt>(
            "window", &m_scalar, 1, &m_windowQuantity, this);
        (*m_windowArgument)[0] = 3;
        addArgument(m_windowArgument.get());
    }

    bool initializeArguments(std::string &message) override
    {
        // A composition may set any number of values; read one or refuse.
        if (m_windowArgument->length() != 1)
        {
            message = "window must hold exactly one value, not " +
                      std::to_string(m_windowArgument->length());
            return false;
        }

        m_window = (*m_windowArgument)[0];
        if (m_window < 1)
        {
            message = "window must be at least 1, not " +
                      std::to_string(m_window);
            return false;
        }

        message.clear();
        return true;
    }
    //! [arguments]

    //! [ports]
    void createInputs() override
    {
        // initialize() empties the base class's lists before calling this
        // again, so assigning a new item here also frees the previous one.
        m_in = std::make_unique<Input1DDouble>("in", &m_scalar, 1,
                                               &m_sampleQuantity, this);
        addInput(m_in.get());
    }

    void createOutputs() override
    {
        m_average = std::make_unique<Output1DDouble>(
            "average", &m_scalar, 1, &m_averageQuantity, this);
        addOutput(m_average.get());
    }

    void initializeFailureCleanUp() override { m_history.clear(); }
    //! [ports]

    //! [members]
  private:
    // What the items point to comes first, so it is destroyed after them.
    // A Quantity does not own its Unit, so the component owns that too.
    Dimension m_scalar{"scalar", "Single value"};
    std::unique_ptr<Unit> m_unitless{Unit::unitlessCoefficient()};
    Quantity m_windowQuantity{"Window", typeid(int), m_unitless.get()};
    Quantity m_sampleQuantity{"Sample", typeid(double), m_unitless.get()};
    Quantity m_averageQuantity{"Average", typeid(double), m_unitless.get()};

    // The items. The base class lists them but never deletes them.
    std::unique_ptr<Argument1DInt> m_windowArgument;
    std::unique_ptr<Input1DDouble> m_in;
    std::unique_ptr<Output1DDouble> m_average;

    int m_window = 3;
    std::deque<double> m_history;
};
//! [members]

//! [info]
/*!
 * \brief Metadata for MovingAverageComponent; creates its instances.
 */
class MovingAverageComponentInfo : public AbstractModelComponentInfo
{
  public:
    MovingAverageComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.docs.moving_average")
    {
        setCaption("Moving Average");
        setDescription("The mean of the last window samples of its input. "
                       "The HydroCoupleComponents tutorial component.");
        setDeveloper("HydroCouple");
        setVersion("0.1.0");
        setTags({"Tutorial"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<MovingAverageComponent>(
            "hydrocouple.components.docs.moving_average.instance");
    }
};
//! [info]

} // namespace

//! [declare]
HYDROCOUPLE_DECLARE_COMPONENT(MovingAverageComponentInfo)
//! [declare]
