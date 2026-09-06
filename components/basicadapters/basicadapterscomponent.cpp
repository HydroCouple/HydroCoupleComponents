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
 */
class ElementwiseAdaptedOutput : public AbstractAdaptedOutput
{
  public:
    ElementwiseAdaptedOutput(std::string_view id, IOutput *adaptee,
                             IAdaptedOutputFactory *factory)
      : AbstractAdaptedOutput(id, {}, nullptr, adaptee, factory)
    {
    }

    void initialize() override
    {
        latchArguments();
        refresh();
    }

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

    void updateValues() override { refresh(); }

    [[nodiscard]] std::vector<int64_t> shape() const override
    {
        IOutput *provider = adaptee();
        return provider ? provider->shape() : std::vector<int64_t>{};
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

    [[nodiscard]] virtual double transform(double value) const = 0;

  private:
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

    std::vector<double> m_values;
};

class NegateAdaptedOutput final : public ElementwiseAdaptedOutput
{
  public:
    using ElementwiseAdaptedOutput::ElementwiseAdaptedOutput;

  protected:
    [[nodiscard]] double transform(double value) const override
    {
        return -value;
    }
};

class ClampAdaptedOutput final : public ElementwiseAdaptedOutput
{
  public:
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
    void latchArguments() override
    {
        m_minimum = m_minimumArgument[0];
        m_maximum = m_maximumArgument[0];
    }

    [[nodiscard]] double transform(double value) const override
    {
        return std::clamp(value, m_minimum, m_maximum);
    }

  private:
    Dimension m_scalar{"scalar"};
    std::unique_ptr<Quantity> m_quantity{Quantity::unitLess("Clamp bound")};

    Argument1DDouble m_minimumArgument;
    Argument1DDouble m_maximumArgument;

    double m_minimum = -1.0e30;
    double m_maximum = 1.0e30;
};

class BasicAdaptersComponentInfo;

class BasicAdaptersComponent final
  : public AbstractAdaptedOutputFactoryComponent
{
  public:
    BasicAdaptersComponent(std::string_view id,
                           AbstractAdaptedOutputFactoryComponentInfo *info)
      : AbstractAdaptedOutputFactoryComponent(id, info)
    {
        m_offerings.push_back(
            std::make_unique<Identity>("negate", "Negate every value"));
        m_offerings.push_back(std::make_unique<Identity>(
            "clamp", "Clamp to [minimum, maximum]"));
    }

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
    std::vector<std::unique_ptr<Identity>> m_offerings;
    int m_created = 0;
};

class BasicAdaptersComponentInfo final
  : public AbstractAdaptedOutputFactoryComponentInfo
{
  public:
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
