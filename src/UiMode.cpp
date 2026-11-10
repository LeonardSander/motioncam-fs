#include "UiMode.h"

#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QVariant>
#include <algorithm>
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QInputDevice>
#else
#include <QTouchDevice>
#endif

namespace motioncam {
namespace {
#ifdef Q_OS_ANDROID
constexpr bool touchEnabled = true;
#else
bool touchEnabled = false;
#endif
}

bool validUiMode(const QString& mode) {
    return mode == "auto" || mode == "desktop" || mode == "touch";
}

bool useTouchUi(const QString& mode, bool touchscreen, const QSize& screen) {
    if (mode == "touch") return true;
    if (mode == "desktop") return false;
    // A touchscreen alone does not make a desktop or convertible a phone.
    return touchscreen && !screen.isEmpty() && std::min(screen.width(), screen.height()) <= 600;
}

QString preferredUiMode() {
    const auto mode = QSettings("com.motioncam", "MotionCam FS")
        .value("interface/mode", "auto").toString();
    return validUiMode(mode) ? mode : QStringLiteral("auto");
}

void initializeUiMode(const QString& mode) {
#ifndef Q_OS_ANDROID
    bool touchscreen = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    for (const auto* device : QInputDevice::devices())
        touchscreen |= device->type() == QInputDevice::DeviceType::TouchScreen;
#else
    for (const auto* device : QTouchDevice::devices())
        touchscreen |= device->type() == QTouchDevice::TouchScreen;
#endif
    const auto* screen = QGuiApplication::primaryScreen();
    touchEnabled = useTouchUi(mode, touchscreen,
        screen ? screen->availableGeometry().size() : QSize());
#else
    Q_UNUSED(mode);
#endif
}

bool touchUi() {
    return touchEnabled;
}

} // namespace motioncam
