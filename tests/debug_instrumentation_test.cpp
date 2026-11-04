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
    for (const char* name : {"MOTIONCAM_GALLERY_PERF_PROFILE",
                            "MOTIONCAM_GALLERY_VALIDATE_GPU",
                            "MOTIONCAM_TEST_FAIL_VULKAN_GAIN_BAKE"}) {
        setFlag(name);
        if (motioncam::debugEnvironmentIsSet(name) != motioncam::debugInstrumentation)
            return 1;
        if (!motioncam::debugInstrumentation && motioncam::debugEnvironment(name))
            return 2;
    }
    const auto zero = std::chrono::steady_clock::time_point{};
    if ((motioncam::diagnosticNow() != zero) != motioncam::debugInstrumentation)
        return 3;
    if ((motioncam::galleryBenchmarkNow() != zero) != motioncam::debugInstrumentation)
        return 4;
    // The explicit benchmark remains usable even in Release.
    motioncam::galleryBenchmarkActive.store(true);
    if (motioncam::galleryBenchmarkNow() == zero)
        return 5;
    if (!motioncam::debugInstrumentation && motioncam::diagnosticNow() != zero)
        return 6;
    std::puts("Instrumentation policy passed");
}
