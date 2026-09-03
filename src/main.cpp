#include "mainwindow.h"
#include "SingleApplication.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>
#include <QFileInfo>
#include <QDirIterator>
#include <QDesktopServices>
#include <QFont>
#include <QGuiApplication>
#include <QProcess>
#include <QPushButton>
#include <QUrl>
#include <algorithm>
#include <spdlog/spdlog.h>

#ifdef __APPLE__
#include "CrashDebug.h"
#include <execinfo.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <exception>
#endif

#ifdef _WIN32
#include <Windows.h>
#endif

namespace {
void messageHandler(QtMsgType type, const QMessageLogContext&, const QString& message) {
    const auto text = message.toLocal8Bit();
    switch (type) {
    case QtDebugMsg: spdlog::debug("Qt: {}", text.constData()); break;
    case QtInfoMsg: spdlog::info("Qt: {}", text.constData()); break;
    case QtWarningMsg: spdlog::warn("Qt: {}", text.constData()); break;
    case QtCriticalMsg: spdlog::error("Qt: {}", text.constData()); break;
    case QtFatalMsg: spdlog::critical("Qt: {}", text.constData()); std::abort();
    }
}

#ifdef __APPLE__
int crashLogFd = -1;
void crashHandler(int signal) {
    const char header[] = "\n==== MotionCamFuse crash ====\n";
    if (crashLogFd >= 0) ::write(crashLogFd, header, sizeof(header) - 1);
    motioncam::debug::dumpCrashContext(crashLogFd >= 0 ? crashLogFd : STDERR_FILENO);
    void* stack[64];
    const int count = ::backtrace(stack, 64);
    ::backtrace_symbols_fd(stack, count, crashLogFd >= 0 ? crashLogFd : STDERR_FILENO);
    if (crashLogFd >= 0) ::fsync(crashLogFd);
    _Exit(128 + signal);
}
void installCrashHandler() {
    const QString directory = QDir::home().filePath("Library/Logs/MotionCam Tools");
    QDir().mkpath(directory);
    crashLogFd = ::open(QDir(directory).filePath("crash.txt").toUtf8().constData(),
                        O_CREAT | O_WRONLY | O_APPEND, 0644);
    struct sigaction action {};
    action.sa_handler = crashHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART | SA_RESETHAND;
    for (int signal : {SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE})
        sigaction(signal, &action, nullptr);
    std::set_terminate([] { crashHandler(SIGABRT); });
}

bool platformReady() {
    const QStringList candidates{"/Library/Filesystems/macfuse.fs",
        "/Library/Frameworks/macfuse.framework", "/Library/Extensions/macfuse.kext",
        "/Library/Frameworks/fuse_t.framework"};
    for (const auto& path : candidates) if (QFileInfo::exists(path)) return true;
    QMessageBox prompt(QMessageBox::Warning, "macFUSE Required",
        "MotionCam Fuse needs macFUSE to mount clips. Install and approve macFUSE in "
        "System Settings > Privacy & Security, then reopen the application.",
        QMessageBox::NoButton);
    auto* download = prompt.addButton("Open macFUSE Download", QMessageBox::AcceptRole);
    auto* privacy = prompt.addButton("Open Privacy & Security", QMessageBox::ActionRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(download);
    prompt.exec();
    if (prompt.clickedButton() == download)
        QDesktopServices::openUrl(QUrl("https://macfuse.github.io/"));
    else if (prompt.clickedButton() == privacy)
        QDesktopServices::openUrl(QUrl(
            "x-apple.systempreferences:com.apple.preference.security?Privacy_Security"));
    return false;
}
#elif defined(_WIN32)
bool platformReady() {
    HMODULE library = LoadLibraryW(L"projectedfslib.dll");
    if (library) { FreeLibrary(library); return true; }
    const QString script = QDir(QCoreApplication::applicationDirPath()).filePath("Enable_ProjFS.bat");
    QMessageBox prompt(QMessageBox::Warning, "ProjectedFS Required",
        "MotionCam Fuse needs Windows Projected File System. Enable it as administrator, "
        "then restart Windows.", QMessageBox::NoButton);
    auto* enable = prompt.addButton("Enable ProjectedFS (Admin)", QMessageBox::AcceptRole);
    auto* openFolder = prompt.addButton("Open Setup Folder", QMessageBox::ActionRole);
    prompt.addButton(QMessageBox::Cancel);
    prompt.setDefaultButton(enable);
    prompt.exec();
    if (prompt.clickedButton() == openFolder) {
        QProcess::startDetached("explorer.exe",
            {QDir::toNativeSeparators(QFileInfo(script).absolutePath())});
        return false;
    }
    if (prompt.clickedButton() == enable) {
        if (!QFileInfo::exists(script)) {
            QMessageBox::critical(nullptr, "ProjectedFS Setup", "Enable_ProjFS.bat was not found.");
        } else {
            const QString command = QString("Start-Process -FilePath '%1' -Verb RunAs")
                                        .arg(QDir::toNativeSeparators(script));
            QProcess::startDetached("powershell.exe",
                {"-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", command});
        }
    }
    return false;
}
#endif
} // namespace

int main(int argc, char *argv[])
{
    SingleApplication app(argc, argv);

#ifdef __APPLE__
    installCrashHandler();
#endif

    // Set application properties
    app.setApplicationName("MotionCam Fuse");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("MotionCam");
#ifdef __APPLE__
    app.setWindowIcon(QIcon(":/assets/app_icon_mac.png"));
    QFont appFont = app.font();
    if (appFont.pointSizeF() > 0) {
        appFont.setPointSizeF(appFont.pointSizeF() + 2.0);
        app.setFont(appFont);
    }
#else
    app.setWindowIcon(QIcon(":/assets/app_icon.png"));
#endif

    // Load theme
    QFile themeFile(":qdarkstyle/dark/darkstyle.qss");

    if (themeFile.open(QFile::ReadOnly | QFile::Text)) {
        QTextStream ts(&themeFile);
        app.setStyleSheet(ts.readAll());
    }

    // Parse command line arguments
    QCommandLineParser parser;

    parser.setApplicationDescription("MotionCam Fuse");
    parser.addHelpOption();
    parser.addVersionOption();

    // Add file option
    QCommandLineOption fileOption(QStringList() << "f" << "file",
                                  "Mount file on startup",
                                  "filename");
    QCommandLineOption galleryPerfOption(
        QStringList() << "gallery-perf-session",
        "Load a session and automate gallery playback diagnostics",
        "session");
    QCommandLineOption galleryPerfPlaybackOption(
        QStringList() << "gallery-perf-playback-ms",
        "Playback sample duration before and after each seek",
        "milliseconds", "3000");

    parser.addOption(fileOption);
    parser.addOption(galleryPerfOption);
    parser.addOption(galleryPerfPlaybackOption);
    parser.process(app);

    // Get file parameter if provided
    QString fileToMount;
    const QString galleryPerfSession = parser.value(galleryPerfOption);
    bool galleryPerfDurationOk = false;
    const int galleryPerfPlaybackMs = parser.value(galleryPerfPlaybackOption)
        .toInt(&galleryPerfDurationOk);
    if (!galleryPerfSession.isEmpty())
        qputenv("MOTIONCAM_DIRECTLOG_DIAGNOSTICS", "1");

    if (parser.isSet(fileOption)) {
        fileToMount = parser.value(fileOption);
    }

    // Check if another instance is running
    if (!app.listen()) {
        if (!fileToMount.isEmpty()) {
            QString message = QString("MOUNT_FILE:%1").arg(fileToMount);
            if (app.sendMessage(message)) {
                return 0; // Successfully sent message to existing instance
            }
        }

        QMessageBox::information(nullptr,
                                 "Application Already Running",
                                 "Another instance of the application is already running.");
        return 1;
    }

#if defined(_WIN32) || defined(__APPLE__)
    if (!platformReady()) return 1;
#endif

    // Create main window
    MainWindow window;
    qInstallMessageHandler(messageHandler);

    // Handle messages from other instances
    QObject::connect(&app, &SingleApplication::messageReceived, &window,
        [&window](const QString &message) {
             if (message.startsWith("MOUNT_FILE:")) {
                 QString filePath = message.mid(11); // Remove "MOUNT_FILE:" prefix

                 window.mountFile(filePath);
                 window.show();
                 window.raise();
                 window.activateWindow();
             }
         });

    // Mount file if provided on startup
    if (!fileToMount.isEmpty()) {
        QTimer::singleShot(100, &window, [&window, fileToMount]() {
            window.mountFile(fileToMount);
        });
    }

    window.show();
#ifdef __APPLE__
    QObject::connect(&app, &QGuiApplication::applicationStateChanged, &window,
        [&window](Qt::ApplicationState state) {
            if (state != Qt::ApplicationActive) return;
            if (window.isVisible() && !window.isMinimized()) return;
            window.show();
            if (window.isMinimized()) window.showNormal();
            window.raise();
            window.activateWindow();
        });
#endif
    if (!galleryPerfSession.isEmpty()) {
        QTimer::singleShot(0, &window,
            [&window, galleryPerfSession, galleryPerfPlaybackMs, galleryPerfDurationOk] {
                window.startGalleryPerformanceTest(
                    galleryPerfSession,
                    galleryPerfDurationOk ? std::max(250, galleryPerfPlaybackMs) : 3000);
            });
    } else if (fileToMount.isEmpty())
        QTimer::singleShot(0, &window, [&window] { window.promptToResumeSession(); });
    return app.exec();
}
