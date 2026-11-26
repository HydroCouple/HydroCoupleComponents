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
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * one String argument, `path` (required), and one input, `in`, a unitless
 * rank-1 Float64 item of length 1. initialize() resolves a relative `path`
 * against the reference directory; prepare() creates or truncates the file
 * and writes the header `step,value`; each update() writes
 * `<step>,<value>` with the stream's default formatting (six significant
 * digits); finish() closes the file.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * The component owns the output file stream and closes it in finish() and
 * after a failed initialization. It is driven from one thread at a time.
 */
class RecorderComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.recorder.instance`.
     */
    explicit RecorderComponent(std::string_view id)
      : AbstractModelComponent(id, "Recorder")
    {
    }

    /*!
     * \brief Reports Valid: initialize() already required a path.
     * \returns An empty list.
     */
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    /*!
     * \brief Resets the step count, creates or truncates the file and
     *        writes the header `step,value`.
     *
     * Moves to Updated with the message "prepared". When the file cannot
     * be opened (a missing parent directory, a parent that is a file, no
     * permission), it queues the error "cannot open "\<path\>" for writing"
     * with code 1 and moves to Failed with the message "the recording file
     * would not open". It does not create directories.
     */
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

    /*!
     * \brief Pulls `in` from its provider and appends one row,
     *        `<step>,<value>`.
     *
     * Reports Done with the message "recorded" when the provider's
     * component is Done or Finished, or when `in` has no provider;
     * otherwise Updated. Rows are not flushed until the stream's buffer
     * fills or finish() closes the file.
     */
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

    //! Closes the file, flushing the rows, and moves to Finished.
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        m_file.close();
        setStatus(ComponentStatus::Finished);
    }

    //! The `in` input; registered with the base class, which does not
    //! delete it; null before the first initialize().
    Input1DDouble *in = nullptr;

  protected:
    /*!
     * \brief Creates `path`, empty by default, with the file filter
     *        "CSV (*.csv)" for hosts that offer a file dialog.
     */
    void createArguments() override
    {
        m_argQuantity.reset(Quantity::unitLess("Recorder argument"));

        m_pathArgument = new Argument1DString("path", &m_scalar, 1,
                                              m_argQuantity.get(), this);
        m_pathArgument->addFileFilter("CSV (*.csv)");
        (*m_pathArgument)[0] = "";
        addArgument(m_pathArgument);
    }

    /*!
     * \brief Requires a path and resolves it.
     *
     * A relative path is joined to referenceDirectory() when that is set,
     * and otherwise left relative to the process's working directory.
     * \param[out] message "path is required: the recorder exists to leave
     *                     a file behind" when `path` is empty, or cleared.
     * \returns false when `path` is empty.
     */
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

    /*!
     * \brief Creates `in`: rank 1, length 1, Float64, unitless "Recorded".
     *
     * It accepts any rank-1 Float64 provider and records only its first
     * value.
     */
    void createInputs() override
    {
        m_inQuantity.reset(Quantity::unitLess("Recorded"));
        in = new Input1DDouble("in", &m_scalar, 1, m_inQuantity.get(), this);
        addInput(in);
    }

    //! The component has no outputs.
    void createOutputs() override {}

    //! Closes the file if a previous run left it open.
    void initializeFailureCleanUp() override
    {
        m_file.close();
    }

  private:
    //! The one dimension of `path` and `in`.
    Dimension m_scalar{"scalar"};
    //! Unitless quantity of `path`.
    std::unique_ptr<Quantity> m_argQuantity;
    //! Unitless quantity "Recorded" of `in`.
    std::unique_ptr<Quantity> m_inQuantity;

    //! `path` argument; registered, never deleted.
    Argument1DString *m_pathArgument = nullptr;

    //! The file written: `path`, joined to the reference directory when
    //! relative.
    std::filesystem::path m_resolvedPath;
    //! The open recording, from prepare() to finish().
    std::ofstream m_file;
    //! Rows written since prepare().
    int m_step = 0;
};

/*!
 * \brief Metadata for RecorderComponent, and the factory for its instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.recorder`, caption "Recorder",
 * version 1.0.0, tags Test and Sink.
 */
class RecorderComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
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

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID
     *          `hydrocouple.components.recorder.instance`, owned by the
     *          caller, who destroys it before the library is unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<RecorderComponent>(
            "hydrocouple.components.recorder.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(RecorderComponentInfo)
