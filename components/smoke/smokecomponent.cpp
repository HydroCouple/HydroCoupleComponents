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
 */
class SmokeComponent : public AbstractModelComponent
{
  public:
    explicit SmokeComponent(std::string_view id)
      : AbstractModelComponent(id, "Smoke Component")
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
        (*produced)[0] = 42.0;
        setStatus(ComponentStatus::Done, "produced");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    Output1DDouble *produced = nullptr;

  protected:
    void createArguments() override {}

    bool initializeArguments(std::string &message) override
    {
        message.clear();
        return true;
    }

    void createInputs() override {}

    void createOutputs() override
    {
        m_quantity.reset(Quantity::unitLess("Answer"));
        produced =
            new Output1DDouble("produced", &m_dimension, 1, m_quantity.get(),
                               this);
        addOutput(produced);
    }

    void initializeFailureCleanUp() override {}

  private:
    Dimension m_dimension{"scalar", "Single value"};
    std::unique_ptr<Quantity> m_quantity;
};

/*!
 * \brief Metadata for SmokeComponent.
 */
class SmokeComponentInfo : public AbstractModelComponentInfo
{
  public:
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

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<SmokeComponent>(
            "hydrocouple.components.smoke.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(SmokeComponentInfo)
