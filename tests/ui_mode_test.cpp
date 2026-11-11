#include "UiMode.h"
#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

int main(int argc, char** argv) {
#ifdef Q_OS_ANDROID
    std::fprintf(stderr, "SKIP: Android GUI initialization requires a Java Activity\n");
    return 77;
#endif
    QApplication app(argc, argv);
    struct Scenario {
        const char* mode;
        bool touchscreen;
        QSize screen;
        bool touch;
    };
    const Scenario scenarios[] = {
        {"auto", false, {390, 844}, false},
        {"auto", true, {390, 844}, true},
        {"auto", true, {844, 390}, true},
        {"auto", true, {1920, 1080}, false},
        {"auto", true, {600, 1000}, true},
        {"auto", true, {601, 1000}, false},
        {"auto", true, {}, false},
        {"desktop", true, {390, 844}, false},
        {"touch", false, {1920, 1080}, true},
    };
    for (const auto& scenario : scenarios) {
        if (rawpreamp::useTouchUi(scenario.mode, scenario.touchscreen, scenario.screen) != scenario.touch) {
            std::fprintf(stderr, "Unexpected UI selection: %s, touchscreen=%d, %dx%d\n",
                scenario.mode, scenario.touchscreen, scenario.screen.width(), scenario.screen.height());
            return 1;
        }
    }
    if (rawpreamp::validUiMode("phone") || rawpreamp::validUiMode("") ||
        !rawpreamp::validUiMode("auto")) return 1;
    // Explicit choices must work on CI even when no touch device is attached.
    rawpreamp::initializeUiMode("touch");
    if (!rawpreamp::touchUi()) return 1;
    rawpreamp::initializeUiMode("desktop");
    if (rawpreamp::touchUi()) return 1;
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
    QSettings settings("com.rawpreamp", "RawPreamp");
    settings.setValue("interface/mode", "touch");
    settings.sync();
    // Saving a different preference must not alter the running layout.
    if (rawpreamp::preferredUiMode() != "touch" || rawpreamp::touchUi()) return 1;
    settings.setValue("interface/mode", "invalid");
    settings.sync();
    if (rawpreamp::preferredUiMode() != "auto") return 1;
    return 0;
}
