// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <algorithm>
#include <chrono>
#include <sstream>
#include <string>
#include <vector>

inline constexpr int verbose = 0, debug = 1, info = 2, warning = 3, error = 4, fatal = 5;
namespace cgp { void WriteLog(int level, const std::string& text) noexcept; bool TimingEnabled() noexcept; }
namespace logging
{
class line
{
public:
    explicit line(int level) : m_level(level) {}
    ~line() { cgp::WriteLog(m_level, m_stream.str()); }
    template<class T> line& operator<<(const T& value) { m_stream << value; return *this; }
    line& operator<<(std::ostream& (*fn)(std::ostream&)) { fn(m_stream); return *this; }
private:
    int m_level;
    std::ostringstream m_stream;
};
template<class T> class min_max_avg_periodic_logger
{
public:
    min_max_avg_periodic_logger(int level, std::string label, std::string unit)
        : m_level(level), m_label(std::move(label)), m_unit(std::move(unit)) {}
    void collect_and_log(T value)
    {
        if (!cgp::TimingEnabled()) return;
        m_samples.push_back(value);
        if (m_samples.size() < 300) return;
        std::sort(m_samples.begin(), m_samples.end());
        line(info) << m_label << " p50=" << m_samples[150] << " p95=" << m_samples[285]
            << " p99=" << m_samples[297] << " " << m_unit;
        m_samples.clear();
    }
private:
    int m_level;
    std::string m_label, m_unit;
    std::vector<T> m_samples;
};
class time_delta_periodic_logger
{
public:
    time_delta_periodic_logger(int level, std::string label) : m_stats(level, std::move(label), "us") {}
    void first_point_now() { m_start = std::chrono::steady_clock::now(); }
    void second_point_now_and_log()
    {
        m_stats.collect_and_log(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - m_start).count());
    }
private:
    std::chrono::steady_clock::time_point m_start;
    min_max_avg_periodic_logger<double> m_stats;
};
}
#define BOOST_LOG(level) ::logging::line(level)
