// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Caleb Buahin

/*!
 * \file   timeseriesprovidercomponent.cpp
 * \brief  TimeSeriesProviderComponent — one component, many sources.
 *
 * A forcing series can live in a delimited CSV, a CE-QUAL-W2 \c .npt deck,
 * a SWMM-style \c .dat file — and tomorrow a web service. The consumer
 * should not care: this component reads whichever the \c source argument
 * names, dispatching by extension (or an explicit \c format), and exposes
 * one time-series output per series found, ready for an exchange
 * connection or an \c \@from binding into a consumer's series argument.
 *
 * The read happens while the component initializes, deliberately: the
 * outputs ARE the file's columns, and a host initializes a component
 * precisely to learn its ports. Times are Julian days throughout (the
 * SDK's convention); calendar text is converted here, once, rather than by
 * every consumer.
 *
 * The format readers form a registry in miniature — each is a free
 * function with the same shape, and a new source kind (NetCDF CF, USGS
 * NWIS) is one more entry, not a redesign. NetCDF and web sources are
 * named today and refused with directions, never silently misread.
 */

#include "hydrocouplesdk/componentabi.h"

#include "hydrocouplesdk/io/uriresolver.h"

#include "hydrocouplesdk/component/abstractmodelcomponent.h"
#include "hydrocouplesdk/component/abstractmodelcomponentinfo.h"
#include "hydrocouplesdk/core/dimension.h"
#include "hydrocouplesdk/core/valuedefinition.h"
#include "hydrocouplesdk/data/argument1d.h"
#include "hydrocouplesdk/temporal/timedata.h"
#include "hydrocouplesdk/temporal/timeseriesexchangeitems.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace HydroCouple;
using namespace HydroCouple::SDK;

namespace
{

/*!
 * \brief One series read from a source, before it becomes an output.
 */
struct ParsedSeries
{
    std::string         name;   //!< The output ID the series gets.
    std::vector<double> times;  //!< Julian days.
    std::vector<double> values; //!< One value per time, as read.
};

// ── Small parsing helpers ─────────────────────────────────────────────────

/*!
 * \brief Removes leading and trailing spaces, tabs, carriage returns and
 *        line feeds.
 * \param[in] text Any text.
 * \returns The trimmed copy; empty when \p text is only whitespace.
 */
std::string trimmed(const std::string &text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

/*!
 * \brief Reads a whole token as a number with std::strtod.
 * \param[in]  token The token; surrounding whitespace is ignored.
 * \param[out] value The number; set even when the token has trailing text.
 * \returns true when the trimmed token is nonempty and strtod consumed all
 *          of it, so "1.5" and "2e3" pass and "1.5m" fails.
 */
bool parseNumber(const std::string &token, double &value)
{
    const std::string clean = trimmed(token);
    if (clean.empty())
        return false;
    char *end = nullptr;
    value = std::strtod(clean.c_str(), &end);
    return end == clean.c_str() + clean.size();
}

/*!
 * Calendar text to a Julian day. Two spellings cover the files in the
 * wild: ISO ("2020-01-31 06:30[:00]", 'T' separator accepted) and US
 * ("01/31/2020 06:30"). The time of day is optional in both.
 *
 * The ISO form is taken only when the first '-' is the fifth character,
 * that is, after a four-digit year. Anything after the seconds (fractions,
 * a zone offset) is ignored, and the fields are not range-checked before
 * TimeData::gregorianToJulianDay converts them.
 * \param[in]  token     The text; surrounding whitespace is ignored.
 * \param[out] julianDay The time as a Julian day.
 * \returns false when neither form reads at least a year, month and day.
 */
bool parseDateTime(const std::string &token, double &julianDay)
{
    const std::string clean = trimmed(token);
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char separator = 0;

    if (std::sscanf(clean.c_str(), "%d-%d-%d%c%d:%d:%d", &year, &month, &day,
                    &separator, &hour, &minute, &second) >= 3 &&
        clean.find('-') == 4)
    {
        julianDay = SDK::Temporal::TimeData::gregorianToJulianDay(
            year, month, day, hour, minute, static_cast<double>(second));
        return true;
    }
    hour = minute = second = 0;
    if (std::sscanf(clean.c_str(), "%d/%d/%d %d:%d:%d", &month, &day, &year,
                    &hour, &minute, &second) >= 3)
    {
        julianDay = SDK::Temporal::TimeData::gregorianToJulianDay(
            year, month, day, hour, minute, static_cast<double>(second));
        return true;
    }
    return false;
}

//! A time token is a number (already Julian) or calendar text.
//! \param[in]  token     The text of a time field.
//! \param[out] julianDay The time as a Julian day.
//! \returns false when the token is neither.
bool parseTime(const std::string &token, double &julianDay)
{
    if (parseNumber(token, julianDay))
        return true;
    return parseDateTime(token, julianDay);
}

/*!
 * \brief Splits a line at every comma and trims each field.
 *
 * There is no quoting: a comma always separates fields.
 * \param[in] line One line of a CSV file.
 * \returns The fields; a trailing comma adds no empty field.
 */
std::vector<std::string> splitCsvLine(const std::string &line)
{
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ','))
        fields.push_back(trimmed(field));
    return fields;
}

// ── Format readers ────────────────────────────────────────────────────────

/*!
 * Delimited CSV: the first column is time, every other column one series.
 * A header row is detected by reading, not assumed: a first line whose
 * time field parses as neither number nor calendar text is names.
 *
 * Blank lines and lines starting with '#' are skipped. The first data row
 * fixes the number of series; a series without a header name is called
 * `<stem>_<column>`, counting the value columns from 1.
 * \param[in]  text    The whole file.
 * \param[in]  stem    The file name without directory or extension.
 * \param[out] series  Receives one entry per value column.
 * \param[out] message Why the file was refused: "CSV line \<n\> has fewer
 *                     than two columns", "CSV line \<n\>: cannot read "\<t\>"
 *                     as a time", "CSV line \<n\> has \<k\> values but \<m\>
 *                     series", "CSV line \<n\>: cannot read "\<v\>" as a
 *                     number", or "The CSV holds no data rows".
 * \returns false on the first refused line, or when there are no rows.
 */
bool readCsv(const std::string &text, const std::string &stem,
             std::vector<ParsedSeries> &series, std::string &message)
{
    std::stringstream stream(text);
    std::string line;
    std::vector<std::string> names;
    bool sniffed = false;
    int lineNumber = 0;

    while (std::getline(stream, line))
    {
        ++lineNumber;
        line = trimmed(line);
        if (line.empty() || line[0] == '#')
            continue;
        const std::vector<std::string> fields = splitCsvLine(line);
        if (fields.size() < 2)
        {
            message = "CSV line " + std::to_string(lineNumber) +
                      " has fewer than two columns";
            return false;
        }

        double time = 0.0;
        if (!sniffed)
        {
            sniffed = true;
            if (!parseTime(fields[0], time))
            {
                // A header: the remaining fields name the series.
                names.assign(fields.begin() + 1, fields.end());
                continue;
            }
        }
        else if (!parseTime(fields[0], time))
        {
            message = "CSV line " + std::to_string(lineNumber) +
                      ": cannot read \"" + fields[0] + "\" as a time";
            return false;
        }

        if (series.empty())
        {
            for (size_t column = 1; column < fields.size(); ++column)
            {
                ParsedSeries entry;
                entry.name = column - 1 < names.size()
                                 ? names[column - 1]
                                 : stem + "_" + std::to_string(column);
                series.push_back(std::move(entry));
            }
        }
        if (fields.size() - 1 != series.size())
        {
            message = "CSV line " + std::to_string(lineNumber) + " has " +
                      std::to_string(fields.size() - 1) + " values but " +
                      std::to_string(series.size()) + " series";
            return false;
        }
        for (size_t column = 1; column < fields.size(); ++column)
        {
            double value = 0.0;
            if (!parseNumber(fields[column], value))
            {
                message = "CSV line " + std::to_string(lineNumber) +
                          ": cannot read \"" + fields[column] +
                          "\" as a number";
                return false;
            }
            series[column - 1].times.push_back(time);
            series[column - 1].values.push_back(value);
        }
    }
    if (series.empty())
        message = "The CSV holds no data rows";
    return !series.empty();
}

/*!
 * CE-QUAL-W2 \c .npt: a title line, a header line naming the columns, then
 * whitespace-separated rows of Julian day and values. Blank and \c $ / \c #
 * marker lines are skipped, as W2 decks carry them.
 *
 * A row is data when its first token is a number; the first non-numeric
 * row is the title and any later one replaces the column names. The first
 * data row fixes the number of series. Extra values on a later row are
 * ignored and missing ones are skipped, so series can end up with
 * different lengths.
 * \param[in]  text    The whole file.
 * \param[in]  stem    The file name without directory or extension; names
 *                     unnamed series `<stem>_<column>`.
 * \param[out] series  Receives one entry per value column.
 * \param[out] message "W2 row "\<row\>": cannot read "\<v\>" as a number" or
 *                     "The .npt deck holds no data rows".
 * \returns false on an unreadable value or when there are no rows.
 */
bool readW2Npt(const std::string &text, const std::string &stem,
               std::vector<ParsedSeries> &series, std::string &message)
{
    std::stringstream stream(text);
    std::string line;
    bool titleSeen = false;
    std::vector<std::string> names;

    while (std::getline(stream, line))
    {
        line = trimmed(line);
        if (line.empty() || line[0] == '$' || line[0] == '#')
            continue;

        std::vector<std::string> tokens;
        std::stringstream fields(line);
        std::string token;
        while (fields >> token)
            tokens.push_back(token);

        double time = 0.0;
        if (!parseNumber(tokens[0], time))
        {
            if (!titleSeen)
            {
                titleSeen = true; // The deck's title.
                continue;
            }
            // The header row: first token labels the day column.
            names.assign(tokens.begin() + 1, tokens.end());
            continue;
        }

        if (series.empty())
        {
            for (size_t column = 1; column < tokens.size(); ++column)
            {
                ParsedSeries entry;
                entry.name = column - 1 < names.size()
                                 ? names[column - 1]
                                 : stem + "_" + std::to_string(column);
                series.push_back(std::move(entry));
            }
        }
        for (size_t column = 1;
             column < tokens.size() && column - 1 < series.size(); ++column)
        {
            double value = 0.0;
            if (!parseNumber(tokens[column], value))
            {
                message = "W2 row \"" + line + "\": cannot read \"" +
                          tokens[column] + "\" as a number";
                return false;
            }
            series[column - 1].times.push_back(time);
            series[column - 1].values.push_back(value);
        }
    }
    if (series.empty())
        message = "The .npt deck holds no data rows";
    return !series.empty();
}

/*!
 * SWMM-style \c .dat: \c ; comments, then "date time value" rows where a
 * two-token row reuses the date of the row before it — exactly how SWMM's
 * own external time-series files carry a day of readings. One series,
 * named after the file.
 *
 * Tokens after the third are ignored. The date and time are read together
 * by parseDateTime(), so both calendar forms work.
 * \param[in]  text    The whole file.
 * \param[in]  stem    The file name without directory or extension; the
 *                     series name.
 * \param[out] series  Receives the one series.
 * \param[out] message "Cannot read .dat line "\<line\>": expected date time
 *                     value", "Cannot read "\<date\> \<time\>" as a date and
 *                     time", "Cannot read "\<v\>" as a number" or "The .dat
 *                     file holds no data rows".
 * \returns false on the first refused line, or when there are no rows.
 */
bool readSwmmDat(const std::string &text, const std::string &stem,
                 std::vector<ParsedSeries> &series, std::string &message)
{
    std::stringstream stream(text);
    std::string line;
    std::string carriedDate;
    ParsedSeries entry;
    entry.name = stem;

    while (std::getline(stream, line))
    {
        line = trimmed(line);
        if (line.empty() || line[0] == ';')
            continue;

        std::vector<std::string> tokens;
        std::stringstream fields(line);
        std::string token;
        while (fields >> token)
            tokens.push_back(token);

        std::string date, time, value;
        if (tokens.size() >= 3)
        {
            date  = tokens[0];
            time  = tokens[1];
            value = tokens[2];
            carriedDate = date;
        }
        else if (tokens.size() == 2 && !carriedDate.empty())
        {
            date  = carriedDate;
            time  = tokens[0];
            value = tokens[1];
        }
        else
        {
            message = "Cannot read .dat line \"" + line +
                      "\": expected date time value";
            return false;
        }

        double julianDay = 0.0, number = 0.0;
        if (!parseDateTime(date + " " + time, julianDay))
        {
            message = "Cannot read \"" + date + " " + time +
                      "\" as a date and time";
            return false;
        }
        if (!parseNumber(value, number))
        {
            message = "Cannot read \"" + value + "\" as a number";
            return false;
        }
        entry.times.push_back(julianDay);
        entry.values.push_back(number);
    }
    if (entry.times.empty())
    {
        message = "The .dat file holds no data rows";
        return false;
    }
    series.push_back(std::move(entry));
    return true;
}

/*!
 * USGS NWIS JSON (instantaneous or daily values service): each
 * \c value.timeSeries entry becomes one output named
 * "\<siteCode\>_\<parameterCode\>". Timestamps are taken as given (the
 * service's local-time offsets are not re-based — a consumer joining
 * series from two zones converts once, knowingly, rather than being
 * handed a silent shift).
 *
 * An entry without `sourceInfo.siteCode` takes the site `site`, and one
 * without `variable.variableCode` the parameter `param`, so such an entry
 * is named `site_param`. Only the first block of an entry's `values` is
 * read; an entry without values, or with an empty first block, is skipped.
 * Each value is a string read as a number.
 * \param[in]  text    The service's answer.
 * \param[out] series  Receives one entry per series with readings.
 * \param[out] message "The service's answer is not JSON: <parser
 *                     message>", "The answer carries no value.timeSeries
 *                     block; is this the NWIS JSON service?", "Cannot read
 *                     "\<stamp\>" as a date and time", "Cannot read "\<v\>" as
 *                     a number", or "The service answered, but with no
 *                     series — check the site and parameter codes".
 * \returns false on any of those.
 */
bool readNwisJson(const std::string &text, std::vector<ParsedSeries> &series,
                  std::string &message)
{
    nlohmann::json document;
    try
    {
        document = nlohmann::json::parse(text);
    }
    catch (const std::exception &e)
    {
        message = std::string("The service's answer is not JSON: ") +
                  e.what();
        return false;
    }

    if (!document.contains("value") ||
        !document["value"].contains("timeSeries"))
    {
        message = "The answer carries no value.timeSeries block; is this "
                  "the NWIS JSON service?";
        return false;
    }
    for (const auto &entry : document["value"]["timeSeries"])
    {
        ParsedSeries parsed;
        std::string site = "site", parameter = "param";
        if (entry.contains("sourceInfo") &&
            entry["sourceInfo"].contains("siteCode") &&
            !entry["sourceInfo"]["siteCode"].empty())
            site = entry["sourceInfo"]["siteCode"][0].value("value", site);
        if (entry.contains("variable") &&
            entry["variable"].contains("variableCode") &&
            !entry["variable"]["variableCode"].empty())
            parsed.name = site + "_" +
                          entry["variable"]["variableCode"][0].value(
                              "value", parameter);
        else
            parsed.name = site + "_" + parameter;

        if (!entry.contains("values") || entry["values"].empty())
            continue;
        for (const auto &row : entry["values"][0].value(
                 "value", nlohmann::json::array()))
        {
            double julianDay = 0.0, number = 0.0;
            const std::string stamp = row.value("dateTime", "");
            if (!parseDateTime(stamp, julianDay))
            {
                message = "Cannot read \"" + stamp + "\" as a date and time";
                return false;
            }
            if (!parseNumber(row.value("value", ""), number))
            {
                message = "Cannot read \"" + row.value("value", "") +
                          "\" as a number";
                return false;
            }
            parsed.times.push_back(julianDay);
            parsed.values.push_back(number);
        }
        if (!parsed.times.empty())
            series.push_back(std::move(parsed));
    }
    if (series.empty())
        message = "The service answered, but with no series — check the "
                  "site and parameter codes";
    return !series.empty();
}

// ── The component ─────────────────────────────────────────────────────────

/*!
 * \brief Reads one source and serves each of its series as an output.
 *
 * A HydroCouple::IModelComponent on the SDK's AbstractModelComponent with
 * three String arguments (`source`, `format`, `cache`), no inputs, and one
 * SDK Temporal::TimeSeriesOutputDouble per series, named after the series,
 * with a unitless quantity of the same name and times in Julian days. The
 * source is read during initialize(), because the outputs are its series;
 * update() changes nothing.
 *
 * Relative `source` and `cache` paths are opened relative to the process's
 * working directory, not the reference directory. Web sources go through
 * the SDK's process-wide UriResolver, which the host installs.
 *
 * Ownership: it creates its arguments and exchange items with `new` and
 * only registers them with the base class, which lists but never deletes
 * them, so they are not freed when the instance is destroyed, and each
 * further initialize() leaves the previous ports behind. It owns the
 * quantities and dimensions they point to; each Quantity::unitLess()
 * quantity also allocates a Unit that nothing frees.
 * It is driven from one thread at a time.
 */
class TimeSeriesProviderComponent : public AbstractModelComponent
{
  public:
    /*!
     * \brief Creates the component in the Created state.
     * \param[in] id The instance ID; the info passes
     *               `hydrocouple.components.timeseriesprovider.instance`.
     */
    explicit TimeSeriesProviderComponent(std::string_view id)
      : AbstractModelComponent(id, "Time Series Provider")
    {
    }

    /*!
     * \brief Reports Valid: the source was read and checked while the
     *        component initialized.
     * \returns An empty list.
     */
    std::vector<std::string> validate() override
    {
        setStatus(ComponentStatus::Validating);
        setStatus(ComponentStatus::Valid);
        return {};
    }

    //! Moves to Updated with the message "prepared".
    void prepare() override
    {
        setStatus(ComponentStatus::Preparing);
        setStatus(ComponentStatus::Updated, "prepared");
    }

    /*!
     * \brief Reports Done with the message "series served".
     *
     * It moves from Updated to Done without passing through Updating, a
     * transition the interface's table rejects, so the SDK queues "illegal
     * status transition Updated -> Done" on errors() at the first update.
     */
    void update(const std::vector<IOutput *> & = {}) override
    {
        // The series were read while the ports were made; a provider's run
        // simply stands behind them.
        setStatus(ComponentStatus::Done, "series served");
    }

    /*!
     * \brief Moves through Finishing to Finished; the outputs keep their
     *        series.
     */
    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    //! `source`, a file path or URL; registered, never deleted.
    SDK::Argument1DString *source = nullptr;
    //! `format`, the reader to use; registered, never deleted.
    SDK::Argument1DString *format = nullptr;
    //! `cache`, the web answer's file; registered, never deleted.
    SDK::Argument1DString *cache  = nullptr;

  protected:
    /*!
     * \brief Creates `source` (empty, with file filters for .csv, .npt and
     *        .dat), `format` ("auto", with a Quality listing auto, csv, w2,
     *        swmm and nwis) and the optional `cache` (empty).
     */
    void createArguments() override
    {
        m_text.reset(Quantity::unitLess("Source"));
        source = new SDK::Argument1DString("source", &m_scalarDimension, 1,
                                           m_text.get(), this);
        (*source)[0] = std::string();
        source->addFileFilter("Time Series (*.csv *.npt *.dat)");
        source->addFileFilter("CSV Files (*.csv)");
        source->addFileFilter("CE-QUAL-W2 Series (*.npt)");
        source->addFileFilter("SWMM Series (*.dat)");
        addArgument(source);

        m_formatQuality = std::make_unique<Quality>(
            "Format", std::vector<std::string>{"auto", "csv", "w2", "swmm",
                                               "nwis"});
        format = new SDK::Argument1DString("format", &m_scalarDimension, 1,
                                           m_formatQuality.get(), this);
        (*format)[0] = std::string("auto");
        addArgument(format);

        // Where a web fetch is kept for offline reruns: the run that
        // fetched and the rerun on a plane read the same numbers.
        cache = new SDK::Argument1DString("cache", &m_scalarDimension, 1,
                                          m_text.get(), this);
        (*cache)[0] = std::string();
        cache->setIsOptional(true);
        addArgument(cache);
    }

    /*!
     * \brief Reads the source into the series the outputs will serve.
     *
     * An empty `source`, after trimming, is a valid provider with no
     * outputs. `format` is trimmed and matched exactly, case included.
     * \param[out] message The reader's or fetch's message on failure, or
     *                     cleared.
     * \returns false when the source cannot be read.
     */
    bool initializeArguments(std::string &message) override
    {
        m_series.clear();
        const std::string path = trimmed((*source)[0]);
        if (path.empty())
        {
            // No source is a valid provider with no series yet — a host
            // initializes components just to see what they are.
            message.clear();
            return true;
        }
        return readSource(path, trimmed((*format)[0]), message);
    }

    //! The component has no inputs.
    void createInputs() override {}

    /*!
     * \brief Creates one TimeSeriesOutputDouble per series read, in the
     *        order the source holds them, each with one time step per row.
     */
    void createOutputs() override
    {
        for (const ParsedSeries &entry : m_series)
        {
            m_quantities.emplace_back(Quantity::unitLess(entry.name));
            auto *output = new SDK::Temporal::TimeSeriesOutputDouble(
                entry.name, {&m_timeDimension}, m_quantities.back().get(),
                this);
            for (size_t row = 0; row < entry.times.size(); ++row)
                output->addTimeStep(std::make_shared<SDK::Temporal::TimeData>(
                                        "Time", entry.times[row]),
                                    {entry.values[row]});
            addOutput(output);
        }
    }

    //! Drops whatever the failed read collected.
    void initializeFailureCleanUp() override { m_series.clear(); }

  private:
    /*!
     * A web source travels through the process UriResolver seam — the SDK
     * carries no transport of its own, and whichever host is running
     * (Composer's HTTP resolver, a test's stub) supplies it. A successful
     * fetch is written to the cache argument's path; a failed one falls
     * back to that cache, so a rerun offline serves the numbers the run
     * that fetched them saw. Refreshing is explicit: reinitialize with the
     * network back.
     *
     * With `format` empty or `auto`, a URL containing `usgs.gov` or `/nwis/`
     * is NWIS; another URL is refused. The cache file is overwritten on
     * every successful fetch and is not created in a missing directory.
     * \param[in]  url        The http or https URL.
     * \param[in]  formatName The trimmed `format` value.
     * \param[out] message    "Unrecognized web service "\<url\>"; the
     *                        registry knows USGS NWIS — set the format
     *                        argument to "nwis" if this is one", "Web
     *                        sources support only the "nwis" format today",
     *                        "Fetched, but cannot write the cache "\<path\>"",
     *                        "The fetch failed (\<reason\>) and there is no
     *                        cache at "\<path\>"", "The fetch failed
     *                        (\<reason\>) and no cache argument is set to fall
     *                        back on", or a readNwisJson() message.
     * \returns false on any of those.
     */
    [[nodiscard]] bool readWebSource(const std::string &url,
                                     const std::string &formatName,
                                     std::string &message)
    {
        std::string kind = formatName.empty() ? "auto" : formatName;
        if (kind == "auto")
        {
            if (url.find("usgs.gov") != std::string::npos ||
                url.find("/nwis/") != std::string::npos)
                kind = "nwis";
            else
            {
                message = "Unrecognized web service \"" + url +
                          "\"; the registry knows USGS NWIS — set the "
                          "format argument to \"nwis\" if this is one";
                return false;
            }
        }
        if (kind != "nwis")
        {
            message = "Web sources support only the \"nwis\" format today";
            return false;
        }

        const std::string cachePath = trimmed((*cache)[0]);
        std::string payload;
        std::string fetchMessage;
        if (IO::readReference(url, payload, fetchMessage))
        {
            if (!cachePath.empty())
            {
                std::ofstream file(cachePath);
                file << payload;
                if (!file)
                {
                    message = "Fetched, but cannot write the cache \"" +
                              cachePath + "\"";
                    return false;
                }
            }
        }
        else if (!cachePath.empty())
        {
            std::ifstream file(cachePath);
            if (!file)
            {
                message = "The fetch failed (" + fetchMessage +
                          ") and there is no cache at \"" + cachePath +
                          "\"";
                return false;
            }
            std::ostringstream text;
            text << file.rdbuf();
            payload = text.str();
        }
        else
        {
            message = "The fetch failed (" + fetchMessage +
                      ") and no cache argument is set to fall back on";
            return false;
        }

        return readNwisJson(payload, m_series, message);
    }

    /*!
     * \brief Chooses the reader for \p path and runs it.
     *
     * An http or https URL goes to readWebSource(). For a file, `format`
     * empty or `auto` chooses by extension, case-sensitively: `.csv` csv,
     * `.npt` w2, `.dat` swmm, `.nc` the refused netcdf kind. An explicit
     * `netcdf` is refused the same way, and an explicit `nwis` is unknown
     * for a file.
     * \param[in]  path       The trimmed `source` value.
     * \param[in]  formatName The trimmed `format` value.
     * \param[out] message    "Cannot tell the format of "\<path\>" from its
     *                        extension; tried .csv, .npt, .dat, .nc — set
     *                        the format argument explicitly", "NetCDF CF
     *                        time series are not implemented yet; convert
     *                        to CSV, or wait for the registry's next
     *                        entry", "Cannot open "\<path\>"", "Unknown format
     *                        "\<f\>"; the registry knows csv, w2, swmm", or a
     *                        reader's message.
     * \returns false on any of those.
     */
    [[nodiscard]] bool readSource(const std::string &path,
                                  const std::string &formatName,
                                  std::string &message)
    {
        const std::string scheme = IO::uriScheme(path);
        if (scheme == "http" || scheme == "https")
            return readWebSource(path, formatName, message);

        const std::string extension =
            std::filesystem::path(path).extension().string();
        const std::string stem =
            std::filesystem::path(path).stem().string();

        std::string kind = formatName.empty() ? "auto" : formatName;
        if (kind == "auto")
        {
            if (extension == ".csv")      kind = "csv";
            else if (extension == ".npt") kind = "w2";
            else if (extension == ".dat") kind = "swmm";
            else if (extension == ".nc")  kind = "netcdf";
            else
            {
                message = "Cannot tell the format of \"" + path +
                          "\" from its extension; tried .csv, .npt, .dat, "
                          ".nc — set the format argument explicitly";
                return false;
            }
        }
        if (kind == "netcdf")
        {
            message = "NetCDF CF time series are not implemented yet; "
                      "convert to CSV, or wait for the registry's next "
                      "entry";
            return false;
        }

        std::ifstream file(path);
        if (!file)
        {
            message = "Cannot open \"" + path + "\"";
            return false;
        }
        std::ostringstream text;
        text << file.rdbuf();

        if (kind == "csv")
            return readCsv(text.str(), stem, m_series, message);
        if (kind == "w2")
            return readW2Npt(text.str(), stem, m_series, message);
        if (kind == "swmm")
            return readSwmmDat(text.str(), stem, m_series, message);

        message = "Unknown format \"" + kind +
                  "\"; the registry knows csv, w2, swmm";
        return false;
    }

    //! The one dimension of the three arguments.
    Dimension m_scalarDimension{"scalar", "Single value"};
    //! The time dimension of every output.
    Dimension m_timeDimension{"time", "Time"};
    //! Unitless quantity of `source` and `cache`.
    std::unique_ptr<Quantity> m_text;
    //! The format names, offered to hosts as choices for `format`.
    std::unique_ptr<Quality>  m_formatQuality;
    //! One unitless quantity per output created; kept across
    //! reinitializations.
    std::vector<std::unique_ptr<Quantity>> m_quantities;
    //! The series read by the last initialization.
    std::vector<ParsedSeries> m_series;
};

/*!
 * \brief Metadata for TimeSeriesProviderComponent, and the factory for its
 *        instances.
 *
 * Published by HYDROCOUPLE_DECLARE_COMPONENT; the library owns it.
 * Component info ID `hydrocouple.components.timeseriesprovider`, caption
 * "Time Series Provider", version 0.1.0, tags Provider and Time Series.
 */
class TimeSeriesProviderComponentInfo : public AbstractModelComponentInfo
{
  public:
    //! Sets the ID, caption, description, developer, version and tags.
    TimeSeriesProviderComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.timeseriesprovider")
    {
        setCaption("Time Series Provider");
        setDescription(
            "Reads time series from CSV, CE-QUAL-W2 .npt, SWMM-style .dat "
            "files, and USGS NWIS web services (cached for offline "
            "reruns), one output per series, for exchange connections "
            "or @from bindings.");
        setDeveloper("HydroCouple");
        setVersion("0.1.0");
        setTags({"Provider", "Time Series"});
    }

    /*!
     * \brief Creates a new instance in the Created state.
     * \returns An instance with ID
     *          `hydrocouple.components.timeseriesprovider.instance`, owned
     *          by the caller, who destroys it before the library is
     *          unloaded.
     */
    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<TimeSeriesProviderComponent>(
            "hydrocouple.components.timeseriesprovider.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(TimeSeriesProviderComponentInfo)
