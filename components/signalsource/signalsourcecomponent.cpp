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
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * five arguments (`waveform`, `amplitude`, `frequency`, `offset`, `steps`),
 * no inputs and one output, `signal`, a unitless rank-1 Float64 item of
 * length 1. initialize() latches the arguments and refuses an unknown
 * waveform or a `steps` below 1; prepare() resets the step count; each
 * update() advances the step, writes the sample, and reports Done on step
 * `steps`. The square wave is `offset + amplitude` wherever the sine is
 * zero or positive. All quantities are unitless; `frequency` is in cycles
 * per step.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * It is driven from one thread at a time.
 */
class SignalSourceComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.signal_source.instance`.
     */
    explicit SignalSourceComponent(std::string_view id)
      : AbstractModelComponent(id, "Signal Source")
    {
    }

    /*!
     * \brief Reports Valid: initialize() already checked the arguments.
     * \returns An empty list.
     */
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    /*!
     * \brief Resets the step count to 0 and moves to Updated with the
     *        message "prepared".
     *
     * Preparing again restarts the waveform at step 1.
     */
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        m_step = 0;
        setStatus(ComponentStatus::Updated, "prepared");
    }

    /*!
     * \brief Advances one step and writes that step's sample to `signal`.
     *
     * Reports Updated with the message "stepped", or Done with the same
     * message once the step count reaches `steps`. It does not check the
     * state, so a host stops calling it once the component is Done.
     */
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

    /*!
     * \brief Moves through Finishing to Finished; there is nothing to
     *        release.
     */
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    //! The `signal` output, created by createOutputs() and registered with the
    //! base class, which does not delete it; null before the first
    //! initialize().
    Output1DDouble *signal = nullptr;

  protected:
    /*!
     * \brief Creates the five arguments with their defaults: `waveform`
     *        "sine", `amplitude` 1, `frequency` 1, `offset` 0 and
     *        `steps` 10.
     *
     * `waveform` carries a Quality listing the four waveforms, so a host
     * can offer them as choices; the argument itself accepts any string,
     * and initializeArguments() refuses the others.
     */
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

    /*!
     * \brief Latches the arguments' first values and checks them.
     * \param[out] message Why the arguments were refused, or cleared.
     * \returns false, with "waveform "\<w\>" is not one of sine, square,
     *          ramp, constant" or "steps must be at least 1, not \<n\>",
     *          when a value is out of range. An argument holding no value
     *          makes the element access throw std::out_of_range instead.
     */
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

    //! The component has no inputs.
    void createInputs() override {}

    /*!
     * \brief Creates the `signal` output: rank 1, length 1, Float64,
     *        unitless quantity "Signal".
     */
    void createOutputs() override
    {
        m_signalQuantity.reset(Quantity::unitLess("Signal"));
        signal = new Output1DDouble("signal", &m_scalar, 1,
                                    m_signalQuantity.get(), this);
        addOutput(signal);
    }

    //! Nothing to release: a refused argument leaves no partial state.
    void initializeFailureCleanUp() override {}

  private:
    //! The one dimension of every argument and of `signal`.
    Dimension m_scalar{"scalar"};
    //! Unitless quantity shared by the numeric arguments.
    std::unique_ptr<Quantity> m_argQuantity;
    //! Unitless quantity "Signal" of the output.
    std::unique_ptr<Quantity> m_signalQuantity;
    //! The four waveform names, offered to hosts as the argument's choices.
    std::unique_ptr<Quality> m_waveformQuality;

    //! `waveform` argument; registered, never deleted.
    Argument1DString *m_waveformArgument = nullptr;
    //! `amplitude` argument; registered, never deleted.
    Argument1DDouble *m_amplitudeArgument = nullptr;
    //! `frequency` argument, cycles per step; registered, never deleted.
    Argument1DDouble *m_frequencyArgument = nullptr;
    //! `offset` argument; registered, never deleted.
    Argument1DDouble *m_offsetArgument = nullptr;
    //! `steps` argument, the run length; registered, never deleted.
    Argument1DInt *m_stepsArgument = nullptr;

    //! Latched waveform name.
    std::string m_waveform = "sine";
    //! Latched amplitude.
    double m_amplitude = 1.0;
    //! Latched frequency, cycles per step.
    double m_frequency = 1.0;
    //! Latched offset.
    double m_offset = 0.0;
    //! Latched run length in steps.
    int m_steps = 10;
    //! Steps taken since prepare(); the sample index n.
    int m_step = 0;
};

/*!
 * \brief Metadata for SignalSourceComponent, and the factory for its
 *        instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.signal_source`, caption
 * "Signal Source", version 1.0.0, tags Test and Source.
 */
class SignalSourceComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
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

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID
     *          `hydrocouple.components.signal_source.instance`, owned by the
     *          caller, who destroys it before the library is unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<SignalSourceComponent>(
            "hydrocouple.components.signal_source.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(SignalSourceComponentInfo)
