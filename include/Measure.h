#pragma once

#include "DebugInstrumentation.h"
#include <chrono>
#include <string>
#include <string_view>
#include <spdlog/spdlog.h>

namespace rawpreamp {

#if RAWPREAMP_DEBUG_INSTRUMENTATION
class Measure {
public:
    explicit Measure(std::string_view name)
        : mName(name)
        , mStart(std::chrono::steady_clock::now()) {
    }

    ~Measure() {
        const auto end = std::chrono::steady_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - mStart).count();

        SPDLOG_DEBUG("{}: {} ms", mName, duration / 1000.0);
    }

    // Prevent copying and moving
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;
    Measure(Measure&&) = delete;
    Measure& operator=(Measure&&) = delete;

private:
    std::string mName;
    std::chrono::time_point<std::chrono::steady_clock> mStart;
};

#else
class Measure {
public:
    explicit Measure(std::string_view) {}
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;
};
#endif

} // namespace rawpreamp
