#include "HardwareVideoEncoder.h"

#include <QCoreApplication>
#include <QFile>
#include <QStandardPaths>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    const auto args = application.arguments();
    // Act as FFmpeg for the selection subprocesses, supplying real encoded
    // fixtures so format validation is exercised independently of GPU access.
    if (args.contains("-hide_banner")) {
        if (args.contains("-h")) {
            if (args.last() == "encoder=hevc_nvenc")
                std::printf("Encoder hevc_nvenc [test]:\r\nSupported pixel formats: yuv444p16le\t p010le\r\n");
            return 0;
        }
        if (qEnvironmentVariableIsSet("ENCODER_TEST_FAIL")) return 1;
        QFile::remove(args.last());
        return QFile::copy(qEnvironmentVariable("ENCODER_TEST_FIXTURE"), args.last()) ? 0 : 1;
    }
    const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty()) return 77;
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    auto fixture = [&](const QString& name, const QString& format, const QString& range) {
        const auto output = directory.filePath(name + ".mov");
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(ffmpeg, {"-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi",
            "-i", "color=black:size=64x64:rate=24", "-frames:v", "1", "-vf", "format=" + format,
            "-c:v", "libx265", "-preset", "ultrafast", "-x265-params", "pools=1:frame-threads=1:log-level=error:colorprim=bt2020",
            "-color_range", range, "-colorspace", "bt2020nc", "-color_primaries", "bt2020", output});
        if (!process.waitForFinished(15000) || process.exitCode()) {
            process.kill();
            process.waitForFinished();
            return QString{};
        }
        return output;
    };
    const auto full10 = fixture("full10", "yuv420p10le", "pc");
    const auto limited10 = fixture("limited10", "yuv420p10le", "tv");
    const auto full8 = fixture("full8", "yuv420p", "pc");
    if (full10.isEmpty() || limited10.isEmpty() || full8.isEmpty()) return 77;
    auto select = [&](const QString& input, const QString& chroma = "420", int width = 64) {
        qputenv("ENCODER_TEST_FIXTURE", input.toUtf8());
        return motioncam::selectHardwareVideoEncoder(application.applicationFilePath(),
            false, chroma, width, 64, 24, 18);
    };
    const auto accepted = select(full10);
    if (accepted.name != "hevc_nvenc" || accepted.pixelFormat != "p010le") return 1;
    if (!select(limited10).name.isEmpty()) return 2;
    if (!select(full8).name.isEmpty()) return 3;
    if (!select(full10, "444").name.isEmpty()) return 4;
    if (!select(full10, "420", 128).name.isEmpty()) return 5;
    qputenv("ENCODER_TEST_FAIL", "1");
    if (!select(full10).name.isEmpty()) return 6;
    return 0;
}
