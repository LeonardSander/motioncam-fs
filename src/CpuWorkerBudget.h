#pragma once

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace motioncam::utils {

// Count idle cores plus cores already used by this process. Other applications
// reduce our budget, while our own parallel work does not throttle itself.
inline unsigned int availableCpuWorkers() {
    const unsigned int cores = std::max(1u, std::thread::hardware_concurrency());
#if defined(__linux__)
    struct Sample {
        std::mutex mutex;
        std::chrono::steady_clock::time_point time{};
        unsigned long long total = 0;
        unsigned long long idle = 0;
        double processSeconds = 0;
        unsigned int budget = 0;
    };
    static Sample sample;
    std::lock_guard lock(sample.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (sample.budget && now - sample.time < std::chrono::milliseconds(250))
        return sample.budget;

    std::ifstream stat("/proc/stat");
    std::string cpu;
    unsigned long long user = 0, nice = 0, system = 0, idle = 0, wait = 0;
    unsigned long long irq = 0, softirq = 0, steal = 0;
    rusage usage{};
    if (!(stat >> cpu >> user >> nice >> system >> idle >> wait >> irq >> softirq >> steal) ||
        cpu != "cpu" || getrusage(RUSAGE_SELF, &usage) != 0)
        return cores;
    const auto total = user + nice + system + idle + wait + irq + softirq + steal;
    const auto idleTicks = idle + wait;
    const double processSeconds = usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 +
                                  usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
    unsigned int budget = cores;
    if (sample.total && total > sample.total && now > sample.time) {
        const double idleCores = static_cast<double>(idleTicks - sample.idle) /
            (total - sample.total) * cores;
        const double seconds = std::chrono::duration<double>(now - sample.time).count();
        const double ownCores = std::max(0.0, (processSeconds - sample.processSeconds) / seconds);
        budget = static_cast<unsigned int>(std::clamp(
            idleCores + ownCores, 1.0, static_cast<double>(cores)));
    }
    sample.time = now;
    sample.total = total;
    sample.idle = idleTicks;
    sample.processSeconds = processSeconds;
    sample.budget = budget;
    return budget;
#else
    return cores;
#endif
}

} // namespace motioncam::utils
