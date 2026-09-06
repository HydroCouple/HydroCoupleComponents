// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   recordercomponent.cpp
 * \brief  A CSV-writing sink — the testing family's terminal component.
 *
 * What it consumes each step lands as a `step,value` row in a file a human
 * can open, so an end-to-end run leaves reviewable evidence instead of
 * values that died inside the process.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/data/exchangeitems1d.h"

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief One input, no outputs, one CSV row per step.
 *
 * Follows its upstream's clock the way the gain transformer does: Done as
 * soon as the connected provider component is Done or Finished.
 */
class RecorderComponent : public AbstractModelComponent
{
  public:
    explicit RecorderComponent(std::string_view id)
      : AbstractModelComponent(id, "Recorder")
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
        m_file.open(m_resolvedPath, std::ios::trunc);

        if (!m_file.is_open())
        {
            reportError(ErrorEntry::Severity::Error, 1,
                        "cannot open \"" + m_resolvedPath.string() +
                            "\" for writing");
            setStatus(ComponentStatus::Failed,
                      "the recording file would not open");
            return;
        }

        m_file << "step,value\n";
        setStatus(ComponentStatus::Updated, "prepared");
    }

    void update(const std::vector<IOutput *> & = {}) override
    {
        setStatus(ComponentStatus::Updating);

        applyInputValues();
        ++m_step;
        m_file << m_step << ',' << (*in)[0] << '\n';

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
                  "recorded");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        m_file.close();
        setStatus(ComponentStatus::Finished);
    }

    Input1DDouble *in = nullptr;

  protected:
    void createArguments() override
    {
        m_argQuantity.reset(Quantity::unitLess("Recorder argument"));

        m_pathArgument = new Argument1DString("path", &m_scalar, 1,
                                              m_argQuantity.get(), this);
        m_pathArgument->addFileFilter("CSV (*.csv)");
        (*m_pathArgument)[0] = "";
        addArgument(m_pathArgument);
    }

    bool initializeArguments(std::string &message) override
    {
        const std::string path = (*m_pathArgument)[0];

        if (path.empty())
        {
            message = "path is required: the recorder exists to leave a "
                      "file behind";
            return false;
        }

        // Relative paths resolve against the composition document, the way
        // every other file argument in the ecosystem does.
        m_resolvedPath = path;
        if (m_resolvedPath.is_relative() && !referenceDirectory().empty())
        {
            m_resolvedPath =
                std::filesystem::path(referenceDirectory()) / path;
        }

        message.clear();
        return true;
    }

    void createInputs() override
    {
        m_inQuantity.reset(Quantity::unitLess("Recorded"));
        in = new Input1DDouble("in", &m_scalar, 1, m_inQuantity.get(), this);
        addInput(in);
    }

    void createOutputs() override {}

    void initializeFailureCleanUp() override
    {
        m_file.close();
    }

  private:
    Dimension m_scalar{"scalar"};
    std::unique_ptr<Quantity> m_argQuantity;
    std::unique_ptr<Quantity> m_inQuantity;

    Argument1DString *m_pathArgument = nullptr;

    std::filesystem::path m_resolvedPath;
    std::ofstream m_file;
    int m_step = 0;
};

class RecorderComponentInfo : public AbstractModelComponentInfo
{
  public:
    RecorderComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.recorder")
    {
        setCaption("Recorder");
        setDescription(
            "Writes one step,value CSV row per step to its path argument. "
            "The testing family's sink: runs leave reviewable evidence.");
        setDeveloper("HydroCouple");
        setVersion("1.0.0");
        setTags({"Test", "Sink"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<RecorderComponent>(
            "hydrocouple.components.recorder.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(RecorderComponentInfo)
