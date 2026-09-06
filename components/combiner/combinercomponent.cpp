// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   combinercomponent.cpp
 * \brief  N providers by role, one combined output.
 *
 * The first concrete AbstractMultiInput anywhere in the ecosystem: every
 * prior input took exactly one provider, so the square multi-input port
 * glyph, the role machinery in applyConnections, and the spec's
 * `to.role` field had nothing real to exercise them.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/identity.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/abstractinput.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/bufferio.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief Two named provider slots — "augend" and "addend" — each taking a
 *        rank-1 Double output. The data plane serves the operands pulled
 *        at the latest retrieve, in providers() order.
 */
class OperandsInput final : public AbstractMultiInput
{
  public:
    OperandsInput(std::string_view id, Dimension *dimension,
                  ValueDefinition *valueDefinition,
                  IModelComponent *component)
      : AbstractMultiInput(id, {dimension}, valueDefinition, component)
    {
        m_labels.push_back(std::make_unique<Identity>("augend", "Augend"));
        m_labels.push_back(std::make_unique<Identity>("addend", "Addend"));
    }

    [[nodiscard]] std::vector<IIdentity *> providerLabels() const override
    {
        std::vector<IIdentity *> labels;
        for (const std::unique_ptr<Identity> &label : m_labels)
        {
            labels.push_back(label.get());
        }
        return labels;
    }

    [[nodiscard]] bool isRequiredProvider(
        const IIdentity *providerLabel) const override
    {
        return providerLabel != nullptr;
    }

    bool canConsume(IOutput *, std::string &message) const override
    {
        message = "the combiner takes providers by role: augend or addend";
        return false;
    }

    bool canConsume(IOutput *provider, std::string &message,
                    const IIdentity *providerRoleIdentifier) const override
    {
        if (!providerRoleIdentifier)
        {
            message = "name a provider role: augend or addend";
            return false;
        }

        const std::string role = providerRoleIdentifier->id();
        if (role != "augend" && role != "addend")
        {
            message = "\"" + role +
                      "\" is not a provider role here; the roles are "
                      "augend and addend";
            return false;
        }

        if (!provider || provider->dataKind() != DataKind::Float64 ||
            provider->shape().size() != 1)
        {
            message = "the " + role +
                      " provider must be a rank-1 Double output";
            return false;
        }

        message.clear();
        return true;
    }

    void retrieveValuesFromProvider() override
    {
        m_collected.clear();

        for (IOutput *provider : providers())
        {
            if (!provider)
            {
                continue;
            }

            double value = 0.0;
            const int64_t one = 1;
            BufferDescriptor destination;
            destination.data = &value;
            destination.kind = DataKind::Float64;
            destination.rank = 1;
            destination.shape = &one;

            const int64_t start = 0;
            if (provider->getValuesInto(destination,
                                        std::span<const int64_t>(&start, 1),
                                        std::span<const int64_t>(&one, 1),
                                        nullptr))
            {
                m_collected.push_back(value);
            }
        }
    }

    void applyData() override {}

    [[nodiscard]] std::vector<int64_t> shape() const override
    {
        return {static_cast<int64_t>(m_collected.size())};
    }

    [[nodiscard]] DataKind dataKind() const override
    {
        return DataKind::Float64;
    }

    [[nodiscard]] bool getValuesInto(const BufferDescriptor &destination,
                                     std::span<const int64_t> start,
                                     std::span<const int64_t> count,
                                     std::string *message =
                                       nullptr) const override
    {
        const int64_t extent = static_cast<int64_t>(m_collected.size());
        return detail::readHyperslab<double>(
            m_collected.data(), std::span<const int64_t>(&extent, 1),
            destination, start, count, message);
    }

    [[nodiscard]] bool setValuesFrom(const BufferDescriptor &,
                                     std::span<const int64_t>,
                                     std::span<const int64_t>,
                                     std::string *message = nullptr) override
    {
        if (message)
        {
            *message = "the operands are pulled from providers, not set";
        }
        return false;
    }

    [[nodiscard]] const std::vector<double> &collected() const
    {
        return m_collected;
    }

  private:
    std::vector<std::unique_ptr<Identity>> m_labels;
    std::vector<double> m_collected;
};

/*!
 * \brief Combines every operand with the configured operation each step.
 *
 * Follows its providers' clocks: Done once every connected provider
 * component is Done or Finished (immediately, when nothing is connected).
 */
class CombinerComponent : public AbstractModelComponent
{
  public:
    explicit CombinerComponent(std::string_view id)
      : AbstractModelComponent(id, "Combiner")
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
        const std::vector<double> &values = operands->collected();

        double result = m_operation == "product" ? 1.0 : 0.0;
        for (const double value : values)
        {
            if (m_operation == "product")
            {
                result *= value;
            }
            else
            {
                result += value;
            }
        }
        if (m_operation == "mean" && !values.empty())
        {
            result /= static_cast<double>(values.size());
        }

        (*combined)[0] = result;

        bool upstreamDone = true;
        for (IOutput *provider : operands->providers())
        {
            if (provider && provider->modelComponent())
            {
                const ComponentStatus status =
                    provider->modelComponent()->status();
                upstreamDone = upstreamDone &&
                               (status == ComponentStatus::Done ||
                                status == ComponentStatus::Finished);
            }
        }

        setStatus(upstreamDone ? ComponentStatus::Done
                               : ComponentStatus::Updated,
                  "combined");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    OperandsInput *operands = nullptr;
    Output1DDouble *combined = nullptr;

  protected:
    void createArguments() override
    {
        m_operationQuality.reset(
            new Quality("Operation", {"sum", "product", "mean"}));

        m_operationArgument = new Argument1DString(
            "operation", &m_scalar, 1, m_operationQuality.get(), this);
        (*m_operationArgument)[0] = "sum";
        addArgument(m_operationArgument);
    }

    bool initializeArguments(std::string &message) override
    {
        m_operation = (*m_operationArgument)[0];

        if (m_operation != "sum" && m_operation != "product" &&
            m_operation != "mean")
        {
            message = "operation \"" + m_operation +
                      "\" is not one of sum, product, mean";
            return false;
        }

        message.clear();
        return true;
    }

    void createInputs() override
    {
        m_operandQuantity.reset(Quantity::unitLess("Operand"));
        operands = new OperandsInput("operands", &m_scalar,
                                     m_operandQuantity.get(), this);
        addInput(operands);
    }

    void createOutputs() override
    {
        m_combinedQuantity.reset(Quantity::unitLess("Combined"));
        combined = new Output1DDouble("combined", &m_scalar, 1,
                                      m_combinedQuantity.get(), this);
        addOutput(combined);
    }

    void initializeFailureCleanUp() override {}

  private:
    Dimension m_scalar{"scalar"};
    std::unique_ptr<Quantity> m_operandQuantity;
    std::unique_ptr<Quantity> m_combinedQuantity;
    std::unique_ptr<Quality> m_operationQuality;

    Argument1DString *m_operationArgument = nullptr;
    std::string m_operation = "sum";
};

class CombinerComponentInfo : public AbstractModelComponentInfo
{
  public:
    CombinerComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.combiner")
    {
        setCaption("Combiner");
        setDescription(
            "Combines providers connected under the augend and addend roles "
            "with sum, product, or mean. The ecosystem's first multi-input "
            "component.");
        setDeveloper("HydroCouple");
        setVersion("1.0.0");
        setTags({"Test", "Transformer"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<CombinerComponent>(
            "hydrocouple.components.combiner.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(CombinerComponentInfo)
