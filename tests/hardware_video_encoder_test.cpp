#include "HardwareVideoEncoder.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

namespace {
// One black HEVC frame per fixture, 64x64 at 24 fps with BT.2020 primaries.
// Generated with libx265 (info=0); embedded so every platform runs this test
// without an external FFmpeg executable or loose binary fixtures.
constexpr char full10Fixture[] =
    "AAAAFGZ0eXBxdCAgAAACAHF0ICAAAAAId2lkZQAAABdtZGF0AAAACygBrHYNn/8UoWpHAAADOm1v"
    "b3YAAABsbXZoZAAAAAAAAAAAAAAAAAAAA+gAAAAqAAEAAAEAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "AAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAKm"
    "dHJhawAAAFx0a2hkAAAAAwAAAAAAAAAAAAAAAQAAAAAAAAAqAAAAAAAAAAAAAAAAAAAAAAABAAAA"
    "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAABAAAAAQAAAAAAAJGVkdHMAAAAcZWxzdAAA"
    "AAAAAAABAAAAKgAAAAAAAQAAAAACHm1kaWEAAAAgbWRoZAAAAAAAAAAAAAAAAAAAMAAAAAIAf/8A"
    "AAAAAC1oZGxyAAAAAG1obHJ2aWRlAAAAAAAAAAAAAAAADFZpZGVvSGFuZGxlcgAAAcltaW5mAAAA"
    "FHZtaGQAAAABAAAAAAAAAAAAAAAsaGRscgAAAABkaGxydXJsIAAAAAAAAAAAAAAAAAtEYXRhSGFu"
    "ZGxlcgAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAV1zdGJsAAAA+XN0c2QA"
    "AAAAAAAAAQAAAOloZXYxAAAAAAAAAAEAAAAARkZNUAAAAgAAAAIAAEAAQABIAAAASAAAAAAAAAAB"
    "FExhdmM2My4xLjEwMiBsaWJ4MjY1AAAAAAAAAAAAAAAAGP//AAAAeWh2Y0MBAiAAAACQAAAAAAAe"
    "8AD8/fr6AAAPAyAAAQAYQAEMAf//AiAAAAMAkAAAAwAAAwAelZQJIQABAC1CAQECIAAAAwCQAAAD"
    "AAADAB6gIIEE2WVlSkwvAW4SBBIIAAADAAgAAAMAwEAiAAEABkQBwHPAiQAAAApmaWVsAQAAAAAQ"
    "cGFzcAAAAAEAAAABAAAAGHN0dHMAAAAAAAAAAQAAAAEAAAIAAAAAHHN0c2MAAAAAAAAAAQAAAAEA"
    "AAABAAAAAQAAABRzdHN6AAAAAAAAAA8AAAABAAAAFHN0Y28AAAAAAAAAAQAAACQAAAAgdWR0YQAA"
    "ABipc3dyAAxVxExhdmY2My4xLjEwMg==";
constexpr char limited10Fixture[] =
    "AAAAFGZ0eXBxdCAgAAACAHF0ICAAAAAId2lkZQAAABdtZGF0AAAACygBrHYNn/7vVakcAAADOm1v"
    "b3YAAABsbXZoZAAAAAAAAAAAAAAAAAAAA+gAAAAqAAEAAAEAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "AAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAKm"
    "dHJhawAAAFx0a2hkAAAAAwAAAAAAAAAAAAAAAQAAAAAAAAAqAAAAAAAAAAAAAAAAAAAAAAABAAAA"
    "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAABAAAAAQAAAAAAAJGVkdHMAAAAcZWxzdAAA"
    "AAAAAAABAAAAKgAAAAAAAQAAAAACHm1kaWEAAAAgbWRoZAAAAAAAAAAAAAAAAAAAMAAAAAIAf/8A"
    "AAAAAC1oZGxyAAAAAG1obHJ2aWRlAAAAAAAAAAAAAAAADFZpZGVvSGFuZGxlcgAAAcltaW5mAAAA"
    "FHZtaGQAAAABAAAAAAAAAAAAAAAsaGRscgAAAABkaGxydXJsIAAAAAAAAAAAAAAAAAtEYXRhSGFu"
    "ZGxlcgAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAV1zdGJsAAAA+XN0c2QA"
    "AAAAAAAAAQAAAOloZXYxAAAAAAAAAAEAAAAARkZNUAAAAgAAAAIAAEAAQABIAAAASAAAAAAAAAAB"
    "FExhdmM2My4xLjEwMiBsaWJ4MjY1AAAAAAAAAAAAAAAAGP//AAAAeWh2Y0MBAiAAAACQAAAAAAAe"
    "8AD8/fr6AAAPAyAAAQAYQAEMAf//AiAAAAMAkAAAAwAAAwAelZQJIQABAC1CAQECIAAAAwCQAAAD"
    "AAADAB6gIIEE2WVlSkwvAWoSBBIIAAADAAgAAAMAwEAiAAEABkQBwHPAiQAAAApmaWVsAQAAAAAQ"
    "cGFzcAAAAAEAAAABAAAAGHN0dHMAAAAAAAAAAQAAAAEAAAIAAAAAHHN0c2MAAAAAAAAAAQAAAAEA"
    "AAABAAAAAQAAABRzdHN6AAAAAAAAAA8AAAABAAAAFHN0Y28AAAAAAAAAAQAAACQAAAAgdWR0YQAA"
    "ABipc3dyAAxVxExhdmY2My4xLjEwMg==";
constexpr char full8Fixture[] =
    "AAAAFGZ0eXBxdCAgAAACAHF0ICAAAAAId2lkZQAAABdtZGF0AAAACygBrHYNn/8UoWpHAAADOG1v"
    "b3YAAABsbXZoZAAAAAAAAAAAAAAAAAAAA+gAAAAqAAEAAAEAAAAAAAAAAAAAAAABAAAAAAAAAAAA"
    "AAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIAAAKk"
    "dHJhawAAAFx0a2hkAAAAAwAAAAAAAAAAAAAAAQAAAAAAAAAqAAAAAAAAAAAAAAAAAAAAAAABAAAA"
    "AAAAAAAAAAAAAAAAAQAAAAAAAAAAAAAAAAAAQAAAAABAAAAAQAAAAAAAJGVkdHMAAAAcZWxzdAAA"
    "AAAAAAABAAAAKgAAAAAAAQAAAAACHG1kaWEAAAAgbWRoZAAAAAAAAAAAAAAAAAAAMAAAAAIAf/8A"
    "AAAAAC1oZGxyAAAAAG1obHJ2aWRlAAAAAAAAAAAAAAAADFZpZGVvSGFuZGxlcgAAAcdtaW5mAAAA"
    "FHZtaGQAAAABAAAAAAAAAAAAAAAsaGRscgAAAABkaGxydXJsIAAAAAAAAAAAAAAAAAtEYXRhSGFu"
    "ZGxlcgAAACRkaW5mAAAAHGRyZWYAAAAAAAAAAQAAAAx1cmwgAAAAAQAAAVtzdGJsAAAA93N0c2QA"
    "AAAAAAAAAQAAAOdoZXYxAAAAAAAAAAEAAAAARkZNUAAAAgAAAAIAAEAAQABIAAAASAAAAAAAAAAB"
    "FExhdmM2My4xLjEwMiBsaWJ4MjY1AAAAAAAAAAAAAAAAGP//AAAAd2h2Y0MBAWAAAACQAAAAAAAe"
    "8AD8/fj4AAAPAyAAAQAYQAEMAf//AWAAAAMAkAAAAwAAAwAelZQJIQABACtCAQEBYAAAAwCQAAAD"
    "AAADAB6gIIEFllZUpMLwFuEgQSCAAAADAIAAAAwEIgABAAZEAcBzwIkAAAAKZmllbAEAAAAAEHBh"
    "c3AAAAABAAAAAQAAABhzdHRzAAAAAAAAAAEAAAABAAACAAAAABxzdHNjAAAAAAAAAAEAAAABAAAA"
    "AQAAAAEAAAAUc3RzegAAAAAAAAAPAAAAAQAAABRzdGNvAAAAAAAAAAEAAAAkAAAAIHVkdGEAAAAY"
    "qXN3cgAMVcRMYXZmNjMuMS4xMDI=";
}

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
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    auto fixture = [&](const QString& name, const char* base64) {
        const auto output = directory.filePath(name + ".mov");
        QFile file(output);
        const auto bytes = QByteArray::fromBase64(base64);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            return QString{};
        return output;
    };
    const auto full10 = fixture("full10", full10Fixture);
    const auto limited10 = fixture("limited10", limited10Fixture);
    const auto full8 = fixture("full8", full8Fixture);
    if (full10.isEmpty() || limited10.isEmpty() || full8.isEmpty()) return 1;
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
