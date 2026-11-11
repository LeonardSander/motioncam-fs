#include "linux/FuseSupport.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>

#include <fstream>
#include <limits>
#include <stdexcept>

namespace {

void testAttributes() {
    const auto check = [](bool condition) {
        if (!condition) throw std::runtime_error("Linux FUSE attributes regression");
    };
    struct stat attributes{};
    for (const size_t size : {size_t(0), size_t(1), size_t(511), size_t(512), size_t(513),
                              size_t(38017601), size_t(std::numeric_limits<off_t>::max())}) {
        rawpreamp::linux_fuse::setReadOnlyAttributes(attributes, false, size, 12345);
        check(S_ISREG(attributes.st_mode) && (attributes.st_mode & 0777) == 0444);
        check(attributes.st_size == static_cast<off_t>(size));
        // Every nonempty generated file must advertise backing blocks to Resolve.
        check(size == 0 ? attributes.st_blocks == 0 : attributes.st_blocks > 0);
        const auto blocks = static_cast<size_t>(attributes.st_blocks);
        check(blocks == size / 512 + (size % 512 != 0));
        check(attributes.st_uid == getuid() && attributes.st_gid == getgid());
        check(attributes.st_mtime == 12345 && attributes.st_nlink == 1);
    }
    // Reusing the stat buffer must clear the previous file's block count.
    rawpreamp::linux_fuse::setReadOnlyAttributes(attributes, true, 0, 12346);
    check(S_ISDIR(attributes.st_mode) && (attributes.st_mode & 0777) == 0755);
    check(attributes.st_size == 4096 && attributes.st_blocks == 0);
    check(attributes.st_nlink == 2 && attributes.st_mtime == 12346);
    check(attributes.st_uid == getuid() && attributes.st_gid == getgid());
}

void testMountSafety() {
    namespace fs = std::filesystem;
    namespace safety = rawpreamp::linux_fuse;
    const auto check = [](bool condition) {
        if (!condition) throw std::runtime_error("Linux FUSE mount safety regression");
    };
    std::istringstream input(
        "45 1 0:44 / /tmp/clip\\040one rw shared:12 - fuse.rawpreamp RawPreamp@123 rw\n"
        "46 1 0:45 / /tmp/other rw - fuse.sshfs remote rw\n"
        "47 1 0:46 / /tmp/legacy rw - fuse /dev/fuse rw\n"
        "malformed\n");
    const auto mounts = safety::readMounts(input);
    check(mounts.size() == 3 && mounts[0].path == "/tmp/clip one");
    check(safety::ownerPid(mounts[0]) == 123);
    check(!safety::ownerPid(mounts[1]) && !safety::ownerPid(mounts[2]));
    check(safety::canRecoverMount(mounts[0], true, -1));
    check(!safety::canRecoverMount(mounts[0], false, -1));
    check(safety::canRecoverMount(mounts[2], false, ENOTCONN));
    for (const int probeError : {0, -1, ENOENT, EACCES})
        check(!safety::canRecoverMount(mounts[2], false, probeError));
    check(!safety::canRecoverMount(mounts[1], true, ENOTCONN));
    for (const auto* source : {"RawPreamp@", "RawPreamp@0", "RawPreamp@-1",
                               "RawPreamp@123abc", "RawPreamp@999999999999999999"})
        check(!safety::ownerPid({"/tmp/clip", "fuse.rawpreamp", source}));
    check(safety::decodeMountField("/tmp/a\\134040b") == "/tmp/a\\040b");
    check(safety::decodeMountField("a\\011b\\012c") == "a\tb\nc");
    check(safety::containsPath("/tmp/clip/", "/tmp/clip/frame.dng"));
    check(!safety::containsPath("/tmp/clip", "/tmp/clip-other"));

    QTemporaryDir directory;
    check(directory.isValid());
    const fs::path root = directory.path().toStdString();
    fs::create_directories(root / "sequence");
    std::ofstream(root / "sequence" / "frame.dng").close();
    fs::create_directory_symlink(root / "sequence", root / "alias");
    auto rejected = [&](const fs::path& source, const fs::path& destination) {
        try { safety::checkSourcePath(fs::weakly_canonical(source),
                                       fs::weakly_canonical(destination)); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    check(rejected(root / "sequence", root / "alias"));
    check(rejected(root / "sequence" / "frame.dng", root / "alias"));
    check(rejected(root / "sequence", root / "alias" / "projection"));
    check(!rejected(root / "sequence", root / "sequence-mounted"));
    check(!rejected(root / "sequence" / "frame.dng", root / "frame-mounted"));
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testAttributes();
    testMountSafety();
    if (argc != 2) throw std::runtime_error("Expected RawPreamp executable path");
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("Cannot create probe fixture");
    const auto probe = [&](const QString& path, int expected) {
        QProcess process;
        process.start(QString::fromLocal8Bit(argv[1]), {"--probe-fuse-mount", path});
        if (!process.waitForStarted(1000) || !process.waitForFinished(5000)) {
            process.kill();
            process.waitForFinished(1000);
            throw std::runtime_error("FUSE probe did not complete");
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != expected)
            throw std::runtime_error("FUSE probe returned an unexpected result");
    };
    probe(directory.path(), 0);
    probe(directory.filePath("missing"), ENOENT);
}
