#pragma once

#include <chrono>
#include <string>
#include <spdlog/spdlog.h>

namespace motioncam {

class Measure {
public:
    explicit Measure(const std::string& name)
        : mName(name)
        , mStart(std::chrono::high_resolution_clock::now()) {
    }

    ~Measure() {
        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - mStart).count();

        spdlog::debug("{}: {} ms", mName, duration / 1000.0);
    }

    // Prevent copying and moving
    Measure(const Measure&) = delete;
    Measure& operator=(const Measure&) = delete;
    Measure(Measure&&) = delete;
    Measure& operator=(Measure&&) = delete;

private:
    std::string mName;
    std::chrono::time_point<std::chrono::high_resolution_clock> mStart;
};

} // namespace motioncam
