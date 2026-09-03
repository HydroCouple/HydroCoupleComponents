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

struct ParsedSeries
{
    std::string         name;
    std::vector<double> times;  //!< Julian days.
    std::vector<double> values;
};

// ── Small parsing helpers ─────────────────────────────────────────────────

std::string trimmed(const std::string &text)
{
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return {};
    const auto end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

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
bool parseTime(const std::string &token, double &julianDay)
{
    if (parseNumber(token, julianDay))
        return true;
    return parseDateTime(token, julianDay);
}

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

// ── The component ─────────────────────────────────────────────────────────

class TimeSeriesProviderComponent : public AbstractModelComponent
{
  public:
    explicit TimeSeriesProviderComponent(std::string_view id)
      : AbstractModelComponent(id, "Time Series Provider")
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
        // The series were read while the ports were made; a provider's run
        // simply stands behind them.
        setStatus(ComponentStatus::Done, "series served");
    }

    void finish() override
    {
        setStatus(ComponentStatus::Finishing);
        setStatus(ComponentStatus::Finished);
    }

    SDK::Argument1DString *source = nullptr;
    SDK::Argument1DString *format = nullptr;

  protected:
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
            "Format", std::vector<std::string>{"auto", "csv", "w2", "swmm"});
        format = new SDK::Argument1DString("format", &m_scalarDimension, 1,
                                           m_formatQuality.get(), this);
        (*format)[0] = std::string("auto");
        addArgument(format);
    }

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

    void createInputs() override {}

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

    void initializeFailureCleanUp() override { m_series.clear(); }

  private:
    [[nodiscard]] bool readSource(const std::string &path,
                                  const std::string &formatName,
                                  std::string &message)
    {
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

    Dimension m_scalarDimension{"scalar", "Single value"};
    Dimension m_timeDimension{"time", "Time"};
    std::unique_ptr<Quantity> m_text;
    std::unique_ptr<Quality>  m_formatQuality;
    std::vector<std::unique_ptr<Quantity>> m_quantities;
    std::vector<ParsedSeries> m_series;
};

/*!
 * \brief Metadata for TimeSeriesProviderComponent.
 */
class TimeSeriesProviderComponentInfo : public AbstractModelComponentInfo
{
  public:
    TimeSeriesProviderComponentInfo()
      : AbstractModelComponentInfo("hydrocouple.components.timeseriesprovider")
    {
        setCaption("Time Series Provider");
        setDescription(
            "Reads time series from CSV, CE-QUAL-W2 .npt, and SWMM-style "
            ".dat files (web services to follow) and provides one output "
            "per series, for exchange connections or @from bindings.");
        setDeveloper("HydroCouple");
        setVersion("0.1.0");
        setTags({"Provider", "Time Series"});
    }

    std::unique_ptr<IModelComponent> createComponentInstance() override
    {
        return std::make_unique<TimeSeriesProviderComponent>(
            "hydrocouple.components.timeseriesprovider.instance");
    }
};

} // namespace

HYDROCOUPLE_DECLARE_COMPONENT(TimeSeriesProviderComponentInfo)
