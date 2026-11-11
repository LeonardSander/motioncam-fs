#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>

// Set per configuration by CMake, including standalone tests with -UNDEBUG.
#ifndef RAWPREAMP_DEBUG_INSTRUMENTATION
# ifdef NDEBUG
#  define RAWPREAMP_DEBUG_INSTRUMENTATION 0
# else
#  define RAWPREAMP_DEBUG_INSTRUMENTATION 1
# endif
#endif

namespace rawpreamp {
inline constexpr bool debugInstrumentation = RAWPREAMP_DEBUG_INSTRUMENTATION != 0;

// Developer overrides must not change the shipped rendering path.
inline const char* debugEnvironment(const char* name) {
#if RAWPREAMP_DEBUG_INSTRUMENTATION
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
#if RAWPREAMP_DEBUG_INSTRUMENTATION
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
} // namespace rawpreamp

#if RAWPREAMP_DEBUG_INSTRUMENTATION
# define RAWPREAMP_DIAGNOSTIC_INFO(...) SPDLOG_INFO(__VA_ARGS__)
#else
# define RAWPREAMP_DIAGNOSTIC_INFO(...) ((void)0)
#endif
