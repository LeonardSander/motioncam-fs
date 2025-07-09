#include "mainwindow.h"
#include "SingleApplication.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QTimer>
#include <QFileInfo>
#include <QDirIterator>

int main(int argc, char *argv[])
{
    SingleApplication app(argc, argv);

    // Set application properties
    app.setApplicationName("MotionCam Fuse");
    app.setApplicationVersion("1.0");
    app.setOrganizationName("MotionCam");
    app.setWindowIcon(QIcon(":/assets/app_icon.png"));

    // Load theme
    QFile themeFile(":qdarkstyle/dark/darkstyle.qss");

    if (themeFile.exists())   {
        themeFile.open(QFile::ReadOnly | QFile::Text);

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

    parser.addOption(fileOption);
    parser.process(app);

    // Get file parameter if provided
    QString fileToMount;

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

    // Create main window
    MainWindow window;

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
    return app.exec();
}
