// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   basicadapterscomponent.cpp
 * \brief  A standalone adapted-output factory, shipped as a plugin.
 *
 * The first IAdaptedOutputFactoryComponent to travel the whole road: built
 * on the SDK bases, published through the stamped ABI, recognized by hosts
 * as a factory (not a model component), realized per run, and reached by
 * ModelInitializer through the standalone-factory resolver seam. Its
 * offerings — negate and clamp — are deliberately disjoint from the SDK's
 * own factory, so a composition that names them proves the LIBRARY was
 * consulted.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/identity.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/abstractadaptedoutput.h"
#include "hydrocouplesdk/data/abstractadaptedoutputfactory.h"
#include "hydrocouplesdk/data/abstractadaptedoutputfactorycomponentinfo.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/bufferio.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief Shared skeleton: pull the adaptee's rank-1 Double slab, transform
 *        it element-wise, serve the result.
 *
 * An SDK AbstractAdaptedOutput, so a HydroCouple::IAdaptedOutput that is
 * itself an output: it can feed an input or another adapted output. Its
 * shape is the adaptee's; its values are the transformed copy made by the
 * last refresh(). Arguments are read once, in initialize(). The adaptee
 * and the factory are not owned; the caller that owns the adapted output
 * unregisters it from its adaptee before destroying it.
 */
class ElementwiseAdaptedOutput : public AbstractAdaptedOutput
{
  public:
    /*!
     * \brief Creates the adapted output over \p adaptee.
     * \param[in] id      The instance ID, `<offering>#<n>`.
     * \param[in] adaptee The output adapted; not owned.
     * \param[in] factory The factory that created it; not owned.
     */
    ElementwiseAdaptedOutput(std::string_view id, IOutput *adaptee,
                             IAdaptedOutputFactory *factory)
      : AbstractAdaptedOutput(id, {}, nullptr, adaptee, factory)
    {
    }

    //! Reads the arguments, then computes the first values.
    void initialize() override
    {
        latchArguments();
        refresh();
    }

    /*!
     * \brief Pulls the adaptee's values, transforms each, and refreshes
     *        the adapted outputs chained to this one.
     *
     * When the adaptee is missing, is not rank 1, or refuses the read, the
     * previous values stay and nothing is signaled.
     */
    void refresh() override
    {
        std::vector<double> source;
        if (!pullAdaptee(source))
        {
            return;
        }

        m_values.resize(source.size());
        for (size_t index = 0; index < source.size(); ++index)
        {
            m_values[index] = transform(source[index]);
        }

        emitValueChanged({0}, {static_cast<int64_t>(m_values.size())});
        refreshAdaptedOutputs();
    }

    //! The same as refresh().
    void updateValues() override { refresh(); }

    /*!
     * \brief The adaptee's shape.
     * \returns The adaptee's shape, or an empty shape without one.
     */
    [[nodiscard]] std::vector<int64_t> shape() const override
    {
        IOutput *provider = adaptee();
        return provider ? provider->shape() : std::vector<int64_t>{};
    }

    //! \returns DataKind::Float64.
    [[nodiscard]] DataKind dataKind() const override
    {
        return DataKind::Float64;
    }

    /*!
     * \brief Copies a hyperslab of the transformed values.
     * \param[in]  destination The caller's buffer.
     * \param[in]  start       First index, rank 1.
     * \param[in]  count       Number of values, rank 1.
     * \param[out] message     Why the copy failed, when not null.
     * \returns false with "adapted output "\<id\>" has no values;
     *          initialize() has not run" while there are no values (also
     *          for an adaptee of length 0), or when the slab or buffer does
     *          not match.
     */
    [[nodiscard]] bool getValuesInto(const BufferDescriptor &destination,
                                     std::span<const int64_t> start,
                                     std::span<const int64_t> count,
                                     std::string *message =
                                       nullptr) const override
    {
        if (m_values.empty())
        {
            if (message)
            {
                *message = "adapted output \"" + id() +
                           "\" has no values; initialize() has not run";
            }
            return false;
        }

        const int64_t extent = static_cast<int64_t>(m_values.size());
        return detail::readHyperslab<double>(
            m_values.data(), std::span<const int64_t>(&extent, 1),
            destination, start, count, message);
    }

    /*!
     * \brief Refuses every write.
     * \param[out] message "adapted outputs are read-only", when not null.
     * \returns false.
     */
    [[nodiscard]] bool setValuesFrom(const BufferDescriptor &,
                                     std::span<const int64_t>,
                                     std::span<const int64_t>,
                                     std::string *message = nullptr) override
    {
        if (message)
        {
            *message = "adapted outputs are read-only";
        }
        return false;
    }

  protected:
    //! Reads argument values into coefficients before the first transform.
    virtual void latchArguments() {}

    /*!
     * \brief The element-wise map.
     * \param[in] value One adaptee value.
     * \returns The adapted value.
     */
    [[nodiscard]] virtual double transform(double value) const = 0;

  private:
    /*!
     * \brief Reads the whole adaptee.
     * \param[out] into Resized to the adaptee's length and filled.
     * \returns false when there is no adaptee, it is not rank 1, or the
     *          read fails.
     */
    bool pullAdaptee(std::vector<double> &into) const
    {
        IOutput *provider = adaptee();
        if (!provider)
        {
            return false;
        }

        const std::vector<int64_t> providerShape = provider->shape();
        if (providerShape.size() != 1 || providerShape[0] < 0)
        {
            return false;
        }

        into.resize(static_cast<size_t>(providerShape[0]));

        BufferDescriptor destination;
        destination.data = into.data();
        destination.kind = DataKind::Float64;
        destination.rank = 1;
        destination.shape = providerShape.data();

        const int64_t start = 0;
        return provider->getValuesInto(
            destination, std::span<const int64_t>(&start, 1),
            std::span<const int64_t>(&providerShape[0], 1), nullptr);
    }

    //! The transformed values of the last successful refresh.
    std::vector<double> m_values;
};

/*!
 * \brief The `negate` offering: every value's negative, `-x`. It has no
 *        arguments.
 */
class NegateAdaptedOutput final : public ElementwiseAdaptedOutput
{
  public:
    using ElementwiseAdaptedOutput::ElementwiseAdaptedOutput;

  protected:
    /*!
     * \brief Negates one value.
     * \param[in] value One adaptee value.
     * \returns `-value`.
     */
    [[nodiscard]] double transform(double value) const override
    {
        return -value;
    }
};

/*!
 * \brief The `clamp` offering: every value limited to [`minimum`,
 *        `maximum`].
 *
 * Its two Float64 arguments, `minimum` (default -1e30) and `maximum`
 * (default 1e30), are members of the adapted output and are read in
 * initialize(). It does not check that `minimum` is at most `maximum`;
 * std::clamp's result is undefined when it is not.
 */
class ClampAdaptedOutput final : public ElementwiseAdaptedOutput
{
  public:
    /*!
     * \brief Creates the adapted output and its two arguments.
     * \param[in] id      The instance ID, `clamp#<n>`.
     * \param[in] adaptee The output adapted; not owned.
     * \param[in] factory The factory that created it; not owned.
     */
    ClampAdaptedOutput(std::string_view id, IOutput *adaptee,
                       IAdaptedOutputFactory *factory)
      : ElementwiseAdaptedOutput(id, adaptee, factory),
        m_minimumArgument("minimum", &m_scalar, 1, m_quantity.get(), nullptr),
        m_maximumArgument("maximum", &m_scalar, 1, m_quantity.get(), nullptr)
    {
        m_minimumArgument[0] = -1.0e30;
        m_maximumArgument[0] = 1.0e30;
        addArgument(&m_minimumArgument);
        addArgument(&m_maximumArgument);
    }

  protected:
    //! Reads the first value of `minimum` and of `maximum`.
    void latchArguments() override
    {
        m_minimum = m_minimumArgument[0];
        m_maximum = m_maximumArgument[0];
    }

    /*!
     * \brief Limits one value to the bounds.
     * \param[in] value One adaptee value.
     * \returns `std::clamp(value, minimum, maximum)`.
     */
    [[nodiscard]] double transform(double value) const override
    {
        return std::clamp(value, m_minimum, m_maximum);
    }

  private:
    //! The one dimension of the two arguments.
    Dimension m_scalar{"scalar"};
    //! Unitless quantity "Clamp bound" of the two arguments.
    std::unique_ptr<Quantity> m_quantity{Quantity::unitLess("Clamp bound")};

    //! `minimum` argument, a member; registered with the base class.
    Argument1DDouble m_minimumArgument;
    //! `maximum` argument, a member; registered with the base class.
    Argument1DDouble m_maximumArgument;

    //! Latched lower bound.
    double m_minimum = -1.0e30;
    //! Latched upper bound.
    double m_maximum = 1.0e30;
};

class BasicAdaptersComponentInfo;

/*!
 * \brief The factory instance: offers `negate` and `clamp` for rank-1
 *        Float64 outputs and creates them.
 *
 * An SDK AbstractAdaptedOutputFactoryComponent, so a
 * HydroCouple::IAdaptedOutputFactoryComponent, not a model component. It
 * owns its two offering identities; it does not own the adapted outputs it
 * creates. Its ID is the info ID, the ID a composition's
 * `adapted_outputs` entries name in their `factory` field.
 */
class BasicAdaptersComponent final
  : public AbstractAdaptedOutputFactoryComponent
{
  public:
    /*!
     * \brief Creates the factory and its two offerings.
     * \param[in] id   The factory ID, `hydrocouple.components.adapters.basic`.
     * \param[in] info The info that created it; owned by the library.
     */
    BasicAdaptersComponent(std::string_view id,
                           AbstractAdaptedOutputFactoryComponentInfo *info)
      : AbstractAdaptedOutputFactoryComponent(id, info)
    {
        m_offerings.push_back(
            std::make_unique<Identity>("negate", "Negate every value"));
        m_offerings.push_back(std::make_unique<Identity>(
            "clamp", "Clamp to [minimum, maximum]"));
    }

    /*!
     * \brief The offerings for \p provider.
     * \param[in] provider The output to adapt.
     * \returns `negate` and `clamp` for a rank-1 Float64 provider of any
     *          length, adapted outputs included; nothing otherwise. The
     *          consumer is not considered.
     */
    std::vector<IIdentity *> getAvailableAdaptedOutputIds(
        const IOutput *provider, const IInput * = nullptr) override
    {
        if (!provider || provider->dataKind() != DataKind::Float64 ||
            provider->shape().size() != 1)
        {
            return {};
        }

        std::vector<IIdentity *> available;
        for (const std::unique_ptr<Identity> &offering : m_offerings)
        {
            available.push_back(offering.get());
        }
        return available;
    }

    /*!
     * \brief Creates one adapted output and registers it with \p provider.
     * \param[in] adaptedProviderId The offering, `negate` or `clamp`.
     * \param[in] provider          The output to adapt; not owned.
     * \returns The adapted output, ID `<offering>#<n>` with n counting this
     *          factory's creations from 1, owned by the caller; null for a
     *          null argument or an unknown offering. The provider's
     *          registration is non-owning: the caller removes it with
     *          removeAdaptedOutput() before destroying the adapted output.
     *          Call the adapted output's initialize() before reading it.
     */
    std::unique_ptr<IAdaptedOutput> createAdaptedOutput(
        IIdentity *adaptedProviderId, IOutput *provider,
        IInput * = nullptr) override
    {
        if (!adaptedProviderId || !provider)
        {
            return nullptr;
        }

        const std::string instanceId =
            adaptedProviderId->id() + "#" + std::to_string(++m_created);

        std::unique_ptr<IAdaptedOutput> created;

        if (adaptedProviderId->id() == "negate")
        {
            created = std::make_unique<NegateAdaptedOutput>(instanceId,
                                                            provider, this);
        }
        else if (adaptedProviderId->id() == "clamp")
        {
            created = std::make_unique<ClampAdaptedOutput>(instanceId,
                                                           provider, this);
        }

        // The interface contract: the returned adapted output is already
        // registered with its provider. The registry is non-owning — the
        // caller unregisters before destroying.
        if (created)
        {
            provider->addAdaptedOutput(created.get());
        }

        return created;
    }

  private:
    //! The offerings `negate` and `clamp`, owned here.
    std::vector<std::unique_ptr<Identity>> m_offerings;
    //! Adapted outputs created so far; numbers their IDs.
    int m_created = 0;
};

/*!
 * \brief Metadata for the factory, and the factory for its instances.
 *
 * An IAdaptedOutputFactoryComponentInfo, not an IModelComponentInfo, so a
 * host lists the library among its adapter factories. Published by
 * HYDROCOUPLE_DECLARE_COMPONENT; the library owns it. Component info ID
 * `hydrocouple.components.adapters.basic`, caption "Basic Adapters",
 * version 1.0.0, tags Test and Adapters.
 */
class BasicAdaptersComponentInfo final
  : public AbstractAdaptedOutputFactoryComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
    BasicAdaptersComponentInfo()
      : AbstractAdaptedOutputFactoryComponentInfo(
          "hydrocouple.components.adapters.basic")
    {
        setCaption("Basic Adapters");
        setDescription(
            "A standalone adapted-output factory offering negate and clamp "
            "over rank-1 Double outputs.");
        setDeveloper("HydroCouple");
        setVersion("1.0.0");
        setTags({"Test", "Adapters"});
    }

    /*!
     * \brief Creates a factory instance.
     * \returns A factory whose ID is this info's ID, owned by the caller,
     *          who destroys it, and every adapted output it made, before the
     *          library is unloaded.
     */
    std::unique_ptr<IAdaptedOutputFactoryComponent>
    createComponentInstance() override
    {
        // The instance carries the info's own id: a composition's
        // adapted_outputs "factory" field matches the realized
        // IAdaptedOutputFactory::id(), and a different ".instance" id
        // there would be a trap for authors.
        return std::make_unique<BasicAdaptersComponent>(
            "hydrocouple.components.adapters.basic", this);
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(BasicAdaptersComponentInfo)
