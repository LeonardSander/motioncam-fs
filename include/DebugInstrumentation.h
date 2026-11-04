#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

// Set per configuration by CMake, including standalone tests with -UNDEBUG.
#ifndef MOTIONCAM_DEBUG_INSTRUMENTATION
# ifdef NDEBUG
#  define MOTIONCAM_DEBUG_INSTRUMENTATION 0
# else
#  define MOTIONCAM_DEBUG_INSTRUMENTATION 1
# endif
#endif

namespace motioncam {
inline constexpr bool debugInstrumentation = MOTIONCAM_DEBUG_INSTRUMENTATION != 0;

// Developer overrides must not change the shipped rendering path.
inline const char* debugEnvironment(const char* name) {
#if MOTIONCAM_DEBUG_INSTRUMENTATION
    return std::getenv(name);
#else
    (void)name;
    return nullptr;
#endif
}
inline bool debugEnvironmentIsSet(const char* name) {
    return debugEnvironment(name) != nullptr;
}
inline std::chrono::steady_clock::time_point diagnosticNow() {
#if MOTIONCAM_DEBUG_INSTRUMENTATION
    return std::chrono::steady_clock::now();
#else
    return {};
#endif
}
// Only the explicit gallery benchmark enables benchmark clocks in Release.
inline std::atomic<bool> galleryBenchmarkActive{false};
inline std::chrono::steady_clock::time_point galleryBenchmarkNow() {
    if (debugInstrumentation || galleryBenchmarkActive.load(std::memory_order_relaxed))
        return std::chrono::steady_clock::now();
    return {};
}
} // namespace motioncam

#if MOTIONCAM_DEBUG_INSTRUMENTATION
# define MOTIONCAM_DIAGNOSTIC_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#else
# define MOTIONCAM_DIAGNOSTIC_INFO(...) ((void)0)
#endif
