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
 *
 * An SDK AbstractMultiInput, so a HydroCouple::IMultiInput: providers are
 * added with a role, and both declared roles are required. A provider is
 * accepted only under a role, and only when it is a rank-1 Float64 output;
 * only its first value is read. Its shape is the number of operands read by
 * the last retrieveValuesFromProvider(), and it cannot be written.
 *
 * The CombinerComponent creates it with `new` and registers it with the
 * base class, which never deletes it. It owns its two role labels.
 */
class OperandsInput final : public AbstractMultiInput
{
  public:
    /*!
     * \brief Creates the input with the roles `augend` and `addend`.
     * \param[in] id              The input ID, `operands`.
     * \param[in] dimension       The one dimension; the component owns it.
     * \param[in] valueDefinition The quantity; the component owns it.
     * \param[in] component       The owning component.
     */
    OperandsInput(std::string_view id, Dimension *dimension,
                  ValueDefinition *valueDefinition,
                  IModelComponent *component)
      : AbstractMultiInput(id, {dimension}, valueDefinition, component)
    {
        m_labels.push_back(std::make_unique<Identity>("augend", "Augend"));
        m_labels.push_back(std::make_unique<Identity>("addend", "Addend"));
    }

    /*!
     * \brief The provider roles, `augend` then `addend`.
     * \returns Non-owning pointers to the two labels.
     */
    [[nodiscard]] std::vector<IIdentity *> providerLabels() const override
    {
        std::vector<IIdentity *> labels;
        for (const std::unique_ptr<Identity> &label : m_labels)
        {
            labels.push_back(label.get());
        }
        return labels;
    }

    /*!
     * \brief Whether a role must be filled.
     * \param[in] providerLabel A role label, or null.
     * \returns true for any non-null label: both roles are required.
     */
    [[nodiscard]] bool isRequiredProvider(
        const IIdentity *providerLabel) const override
    {
        return providerLabel != nullptr;
    }

    /*!
     * \brief Refuses every provider offered without a role.
     * \param[out] message "the combiner takes providers by role: augend or
     *                     addend".
     * \returns false.
     */
    bool canConsume(IOutput *, std::string &message) const override
    {
        message = "the combiner takes providers by role: augend or addend";
        return false;
    }

    /*!
     * \brief Accepts a rank-1 Float64 provider under `augend` or `addend`.
     * \param[in]  provider               The output offered.
     * \param[out] message                Why it was refused, or cleared.
     * \param[in]  providerRoleIdentifier The role it is offered for.
     * \returns false with "name a provider role: augend or addend" for a
     *          null role, ""\<role\>" is not a provider role here; the roles
     *          are augend and addend" for another role, and "the \<role\>
     *          provider must be a rank-1 Double output" for a null, non
     *          Float64 or other-rank provider.
     */
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

    /*!
     * \brief Reads the first value of every connected provider, in
     *        providers() order.
     *
     * A provider whose read fails is skipped, so the operand count can be
     * smaller than the provider count.
     */
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

    //! The component reads collected() itself; nothing to apply.
    void applyData() override {}

    /*!
     * \brief The operand count.
     * \returns `{n}`, n being the number of values the last retrieve read.
     */
    [[nodiscard]] std::vector<int64_t> shape() const override
    {
        return {static_cast<int64_t>(m_collected.size())};
    }

    //! \returns DataKind::Float64.
    [[nodiscard]] DataKind dataKind() const override
    {
        return DataKind::Float64;
    }

    /*!
     * \brief Copies a hyperslab of the collected operands.
     * \param[in]  destination The caller's buffer.
     * \param[in]  start       First index, rank 1.
     * \param[in]  count       Number of values, rank 1.
     * \param[out] message     Why the copy failed, when not null.
     * \returns false when the slab lies outside the operands or the buffer
     *          does not match.
     */
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

    /*!
     * \brief Refuses every write.
     * \param[out] message "the operands are pulled from providers, not set",
     *                     when not null.
     * \returns false.
     */
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

    /*!
     * \brief The operands read by the last retrieve.
     * \returns A reference valid until the next retrieve.
     */
    [[nodiscard]] const std::vector<double> &collected() const
    {
        return m_collected;
    }

  private:
    //! The role labels `augend` and `addend`, owned here.
    std::vector<std::unique_ptr<Identity>> m_labels;
    //! First value of each provider at the last retrieve.
    std::vector<double> m_collected;
};

/*!
 * \brief Combines every operand with the configured operation each step.
 *
 * Follows its providers' clocks: Done once every connected provider
 * component is Done or Finished (immediately, when nothing is connected).
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * one String argument, `operation` (`sum`, `product` or `mean`, default
 * `sum`), the multi-input `operands` (an OperandsInput) and one output,
 * `combined`, a unitless rank-1 Float64 item of length 1. With no operands
 * the sum and the mean are 0 and the product is 1.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * It is driven from one thread at a time.
 */
class CombinerComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.combiner.instance`.
     */
    explicit CombinerComponent(std::string_view id)
      : AbstractModelComponent(id, "Combiner")
    {
    }

    /*!
     * \brief Reports Valid. It does not check that both roles are filled.
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
     * \brief Reads the operands and writes their sum, product or mean to
     *        `combined`.
     *
     * Reports Done with the message "combined" once every connected
     * provider's component is Done or Finished, and Updated before that.
     */
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

    /*!
     * \brief Moves through Finishing to Finished; there is nothing to
     *        release.
     */
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    //! The `operands` multi-input; registered, never deleted.
    OperandsInput *operands = nullptr;
    //! The `combined` output; registered, never deleted.
    Output1DDouble *combined = nullptr;

  protected:
    /*!
     * \brief Creates `operation`, default `sum`, with a Quality listing
     *        the three operations for hosts that offer choices.
     */
    void createArguments() override
    {
        m_operationQuality.reset(
            new Quality("Operation", {"sum", "product", "mean"}));

        m_operationArgument = new Argument1DString(
            "operation", &m_scalar, 1, m_operationQuality.get(), this);
        (*m_operationArgument)[0] = "sum";
        addArgument(m_operationArgument);
    }

    /*!
     * \brief Latches `operation` and checks it.
     * \param[out] message "operation "\<o\>" is not one of sum, product,
     *                     mean" for another value, or cleared.
     * \returns false for an unknown operation.
     */
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

    //! Creates the `operands` multi-input with its two roles.
    void createInputs() override
    {
        m_operandQuantity.reset(Quantity::unitLess("Operand"));
        operands = new OperandsInput("operands", &m_scalar,
                                     m_operandQuantity.get(), this);
        addInput(operands);
    }

    //! Creates `combined`: rank 1, length 1, Float64, unitless "Combined".
    void createOutputs() override
    {
        m_combinedQuantity.reset(Quantity::unitLess("Combined"));
        combined = new Output1DDouble("combined", &m_scalar, 1,
                                      m_combinedQuantity.get(), this);
        addOutput(combined);
    }

    //! Nothing to release: a refused operation leaves no partial state.
    void initializeFailureCleanUp() override {}

  private:
    //! The one dimension of the argument and exchange items.
    Dimension m_scalar{"scalar"};
    //! Unitless quantity "Operand" of `operands`.
    std::unique_ptr<Quantity> m_operandQuantity;
    //! Unitless quantity "Combined" of `combined`.
    std::unique_ptr<Quantity> m_combinedQuantity;
    //! The three operation names, offered to hosts as choices.
    std::unique_ptr<Quality> m_operationQuality;

    //! `operation` argument; registered, never deleted.
    Argument1DString *m_operationArgument = nullptr;
    //! Latched operation name.
    std::string m_operation = "sum";
};

/*!
 * \brief Metadata for CombinerComponent, and the factory for its instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.combiner`, caption "Combiner",
 * version 1.0.0, tags Test and Transformer.
 */
class CombinerComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
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

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID
     *          `hydrocouple.components.combiner.instance`, owned by the
     *          caller, who destroys it before the library is unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<CombinerComponent>(
            "hydrocouple.components.combiner.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(CombinerComponentInfo)
