#include "DebugInstrumentation.h"
#include <cstdio>
#include <initializer_list>

namespace {
void setFlag(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "1");
#else
    setenv(name, "1", 1);
#endif
}
}

int main() {
    // Host environment variables cannot enable instrumentation or test failure
    // injection in a shipped binary, including builds whose tests use -UNDEBUG.
    for (const char* name : {"RAWPREAMP_GALLERY_PERF_PROFILE",
                            "RAWPREAMP_GALLERY_VALIDATE_GPU",
                            "RAWPREAMP_TEST_FAIL_VULKAN_GAIN_BAKE"}) {
        setFlag(name);
        if (rawpreamp::debugEnvironmentIsSet(name) != rawpreamp::debugInstrumentation)
            return 1;
        if (!rawpreamp::debugInstrumentation && rawpreamp::debugEnvironment(name))
            return 2;
    }
    const auto zero = std::chrono::steady_clock::time_point{};
    if ((rawpreamp::diagnosticNow() != zero) != rawpreamp::debugInstrumentation)
        return 3;
    if ((rawpreamp::galleryBenchmarkNow() != zero) != rawpreamp::debugInstrumentation)
        return 4;
    // The explicit benchmark remains usable even in Release.
    rawpreamp::galleryBenchmarkActive.store(true);
    if (rawpreamp::galleryBenchmarkNow() == zero)
        return 5;
    if (!rawpreamp::debugInstrumentation && rawpreamp::diagnosticNow() != zero)
        return 6;
    std::puts("Instrumentation policy passed");
}
