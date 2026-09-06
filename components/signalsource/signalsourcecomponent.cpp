// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   signalsourcecomponent.cpp
 * \brief  A configurable waveform source for exercising compositions.
 *
 * Part of the testing family: a deterministic provider whose output is
 * easy to predict from its arguments, so canvas demos and end-to-end gates
 * can assert exact values instead of hand-waving at "some signal".
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief Emits one sample of a configured waveform per update.
 *
 * value(step) for step = 1..steps:
 *   sine:     offset + amplitude * sin(2π · frequency · step)
 *   square:   offset ± amplitude   (sign of the sine)
 *   ramp:     offset + amplitude * frac(frequency · step)
 *   constant: offset + amplitude
 */
class SignalSourceComponent : public AbstractModelComponent
{
  public:
    explicit SignalSourceComponent(std::string_view id)
      : AbstractModelComponent(id, "Signal Source")
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
        m_step = 0;
        setStatus(ComponentStatus::Updated, "prepared");
    }

    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating);

        ++m_step;
        const double phase = m_frequency * static_cast<double>(m_step);

        double value = m_offset;
        if (m_waveform == "sine")
        {
            value += m_amplitude * std::sin(2.0 * std::numbers::pi * phase);
        }
        else if (m_waveform == "square")
        {
            value += std::sin(2.0 * std::numbers::pi * phase) >= 0.0
                       ? m_amplitude
                       : -m_amplitude;
        }
        else if (m_waveform == "ramp")
        {
            value += m_amplitude * (phase - std::floor(phase));
        }
        else // constant
        {
            value += m_amplitude;
        }

        (*signal)[0] = value;

        setStatus(m_step >= m_steps ? ComponentStatus::Done
                                    : ComponentStatus::Updated,
                  "stepped");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    Output1DDouble *signal = nullptr;

  protected:
    void createArguments() override
    {
        m_argQuantity.reset(Quantity::unitLess("Signal argument"));
        m_waveformQuality.reset(
            new Quality("Waveform", {"sine", "square", "ramp", "constant"}));

        m_waveformArgument = new Argument1DString(
            "waveform", &m_scalar, 1, m_waveformQuality.get(), this);
        (*m_waveformArgument)[0] = "sine";
        addArgument(m_waveformArgument);

        m_amplitudeArgument = new Argument1DDouble(
            "amplitude", &m_scalar, 1, m_argQuantity.get(), this);
        (*m_amplitudeArgument)[0] = 1.0;
        addArgument(m_amplitudeArgument);

        m_frequencyArgument = new Argument1DDouble(
            "frequency", &m_scalar, 1, m_argQuantity.get(), this);
        (*m_frequencyArgument)[0] = 1.0;
        addArgument(m_frequencyArgument);

        m_offsetArgument = new Argument1DDouble("offset", &m_scalar, 1,
                                                m_argQuantity.get(), this);
        (*m_offsetArgument)[0] = 0.0;
        addArgument(m_offsetArgument);

        m_stepsArgument = new Argument1DInt("steps", &m_scalar, 1,
                                            m_argQuantity.get(), this);
        (*m_stepsArgument)[0] = 10;
        addArgument(m_stepsArgument);
    }

    bool initializeArguments(std::string &message) override
    {
        m_waveform = (*m_waveformArgument)[0];
        m_amplitude = (*m_amplitudeArgument)[0];
        m_frequency = (*m_frequencyArgument)[0];
        m_offset = (*m_offsetArgument)[0];
        m_steps = (*m_stepsArgument)[0];

        if (m_waveform != "sine" && m_waveform != "square" &&
            m_waveform != "ramp" && m_waveform != "constant")
        {
            message = "waveform \"" + m_waveform +
                      "\" is not one of sine, square, ramp, constant";
            return false;
        }

        if (m_steps < 1)
        {
            message = "steps must be at least 1, not " +
                      std::to_string(m_steps);
            return false;
        }

        message.clear();
        return true;
    }

    void createInputs() override {}

    void createOutputs() override
    {
        m_signalQuantity.reset(Quantity::unitLess("Signal"));
        signal = new Output1DDouble("signal", &m_scalar, 1,
                                    m_signalQuantity.get(), this);
        addOutput(signal);
    }

    void initializeFailureCleanUp() override {}

  private:
    Dimension m_scalar{"scalar"};
    std::unique_ptr<Quantity> m_argQuantity;
    std::unique_ptr<Quantity> m_signalQuantity;
    std::unique_ptr<Quality> m_waveformQuality;

    Argument1DString *m_waveformArgument = nullptr;
    Argument1DDouble *m_amplitudeArgument = nullptr;
    Argument1DDouble *m_frequencyArgument = nullptr;
    Argument1DDouble *m_offsetArgument = nullptr;
    Argument1DInt *m_stepsArgument = nullptr;

    std::string m_waveform = "sine";
    double m_amplitude = 1.0;
    double m_frequency = 1.0;
    double m_offset = 0.0;
    int m_steps = 10;
    int m_step = 0;
};

class SignalSourceComponentInfo : public AbstractModelComponentInfo
{
  public:
    SignalSourceComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.signal_source")
    {
        setCaption("Signal Source");
        setDescription(
            "Emits one sample of a configured waveform (sine, square, ramp, "
            "or constant) per step. A deterministic source for exercising "
            "compositions.");
        setDeveloper("HydroCouple");
        setVersion("1.0.0");
        setTags({"Test", "Source"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<SignalSourceComponent>(
            "hydrocouple.components.signal_source.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(SignalSourceComponentInfo)
