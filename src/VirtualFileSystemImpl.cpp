#include "VirtualFileSystemImpl.h"
#include "DataLevels.h"
#include "DNGDecoder.h"
#include <motioncam/Decoder.hpp>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <array>
#include <fstream>
#include <filesystem>
#include <QProcess>
#include <QTemporaryDir>
#include <boost/filesystem.hpp>
#include <spdlog/spdlog.h>

namespace motioncam {
namespace vfs {

void finalize(
    IVirtualFileSystem& filesystem,
    const std::string& destination,
    bool jpegCompression,
    const FinalizeOptions& options,
    const std::function<bool(size_t, size_t, const std::string&)>& progress,
    const std::function<void(const std::vector<uint8_t>&, Timestamp)>& fileReady,
    bool writeFiles) {
    namespace stdfs = std::filesystem;

    if (!writeFiles && !fileReady)
        throw std::invalid_argument("Streaming finalization requires a frame callback");

    auto entries = filesystem.listFiles("");
    entries.erase(std::remove_if(entries.begin(), entries.end(), [](const Entry& entry) {
        return entry.type != EntryType::FILE_ENTRY || entry.name == "desktop.ini";
    }), entries.end());

    auto outputPath = [&](const Entry& entry) {
        stdfs::path path(destination);
        for (const auto& part : entry.pathParts) path /= part;
        return path / entry.name;
    };
    auto isDng = [](const Entry& entry) {
        return entry.name.size() >= 4 && entry.name.substr(entry.name.size() - 4) == ".dng";
    };
    auto readBytes = [](const stdfs::path& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) throw std::runtime_error("Could not read " + path.string());
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(stream)), {});
    };
    auto writeBytes = [](const stdfs::path& path, const std::vector<uint8_t>& bytes) {
        stdfs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!stream) throw std::runtime_error("Could not write " + path.string());
    };

    struct Gap { size_t left, right; std::vector<size_t> frames; };
    std::vector<Gap> gaps;
    if (options.interpolateDuplicatedFrames) {
        for (size_t i = 1; i + 1 < entries.size();) {
            if (entries[i].name == "audio.wav" || entries[i - 1].name == "audio.wav" ||
                std::get<int64_t>(entries[i].userData) != std::get<int64_t>(entries[i - 1].userData)) {
                ++i;
                continue;
            }
            Gap gap{i - 1, i + 1, {}};
            while (gap.right < entries.size() && entries[gap.right].name != "audio.wav" &&
                   std::get<int64_t>(entries[gap.right].userData) ==
                       std::get<int64_t>(entries[gap.left].userData))
                ++gap.right;
            if (gap.right == entries.size() || entries[gap.right].name == "audio.wav") break;
            for (size_t frame = i; frame < gap.right; ++frame) gap.frames.push_back(frame);
            gaps.push_back(std::move(gap));
            i = gaps.back().right;
        }
    }
    const bool interpolate = !gaps.empty();
    if (options.interpolateDuplicatedFrames)
        spdlog::info("RIFE interpolation found {} gap(s) containing {} duplicated frame(s)",
            gaps.size(), [&] { size_t count = 0; for (const auto& gap : gaps) count += gap.frames.size(); return count; }());
    if (interpolate && (options.rifeDirectory.empty() ||
        !stdfs::is_regular_file(stdfs::path(options.rifeDirectory) / "inference_img.py")))
        throw std::runtime_error("RIFE interpolation is enabled, but inference_img.py was not found in the configured RIFE directory");

    std::vector<int> gapForRight(entries.size(), -1);
    std::vector<int> lastGapForMember(entries.size(), -1);
    std::vector<bool> gapMember(entries.size(), false);
    std::vector<bool> syntheticMember(entries.size(), false);
    size_t syntheticCount = 0, delayedCompressionCount = 0;
    for (size_t g = 0; g < gaps.size(); ++g) {
        gapForRight[gaps[g].right] = static_cast<int>(g);
        gapMember[gaps[g].left] = gapMember[gaps[g].right] = true;
        lastGapForMember[gaps[g].left] = lastGapForMember[gaps[g].right] = static_cast<int>(g);
        for (const auto frame : gaps[g].frames) {
            gapMember[frame] = true;
            syntheticMember[frame] = true;
            lastGapForMember[frame] = static_cast<int>(g);
        }
        syntheticCount += gaps[g].frames.size();
    }
    if (jpegCompression)
        for (size_t i = 0; i < entries.size(); ++i)
            if (gapMember[i] && entries[i].name.size() >= 4 &&
                entries[i].name.substr(entries[i].name.size() - 4) == ".dng")
                ++delayedCompressionCount;
    const size_t totalWork = entries.size() + syntheticCount + delayedCompressionCount;
    size_t completed = 0;
    size_t nextReady = 0;
    std::vector<bool> ready(entries.size(), false);
    std::vector<std::vector<uint8_t>> finalizedDngs(entries.size());
    auto emitReady = [&] {
        while (nextReady < entries.size() && ready[nextReady]) {
            const auto& entry = entries[nextReady++];
            if (fileReady && entry.name.size() >= 4 &&
                entry.name.substr(entry.name.size() - 4) == ".dng") {
                Timestamp timestamp = 0;
                auto& bytes = finalizedDngs[nextReady - 1];
                if (bytes.empty() && writeFiles) bytes = readBytes(outputPath(entry));
                if (!DNGDecoder::getTimingMetadata(bytes, timestamp))
                    throw std::runtime_error("Finalized DNG is missing timing metadata: " + entry.name);
                fileReady(bytes, timestamp);
                if (writeFiles || !gapMember[nextReady - 1]) {
                    bytes.clear();
                    bytes.shrink_to_fit();
                }
            }
        }
    };
    auto report = [&](const std::string& label) {
        if (progress && !progress(completed, totalWork, label))
            throw std::runtime_error("Finalization cancelled");
    };
    auto compressDng = [&](size_t index) {
        auto dng = writeFiles
            ? readBytes(outputPath(entries[index])) : finalizedDngs[index];
        const bool ok = options.jxlDistance >= 0.0f
            ? DNGDecoder::compressJPEGXL(dng, options.jxlDistance)
            : DNGDecoder::compressLosslessJPEG(dng);
        if (!ok) throw std::runtime_error("Could not compress " + entries[index].name);
        if (writeFiles) writeBytes(outputPath(entries[index]), dng);
        if (fileReady) finalizedDngs[index] = std::move(dng);
    };

    QTemporaryDir staging;
    QProcess rife;
    if (interpolate) {
        if (!staging.isValid()) throw std::runtime_error("Could not create RIFE staging directory");
        const QString code = QString::fromUtf8(R"PY(
import os, sys, types, numpy as np, torch
from torch.nn import functional as F
repo = sys.argv.pop(1)
os.chdir(repo); sys.path.insert(0, repo)
# MotionCam supplies raw tensors directly; RIFE's OpenCV image I/O is unused.
# Avoid importing its binary cv2 wheel, which may target an older NumPy ABI.
sys.modules['cv2'] = types.ModuleType('cv2')
import inference_img as rife
rife.LoadModel(None); rife.StartModel()
print('MOTIONCAM_READY',flush=True)
def load(path,width,height):
    a=np.fromfile(path,dtype='<u2').reshape(height,width,3).astype(np.float32)/65535.0
    t=torch.tensor(a.transpose(2,0,1)).to(rife.DEVICE).unsqueeze(0)
    ph=(height+63)//64*64; pw=(width+63)//64*64
    return F.pad(t,(0,pw-width,0,ph-height)).half() if rife.GPU_FP16 else F.pad(t,(0,pw-width,0,ph-height))
for line in sys.stdin:
    parts=line.rstrip('\n').split('\t'); width,height=int(parts[0]),int(parts[1])
    left,right=parts[2],parts[3]; a,b=load(left,width,height),load(right,width,height)
    for item in parts[4:]:
        ratio,out=item.split('|',1)
        image=rife.RatioSplit(a,b,float(ratio))[0,:,:height,:width]
        pixels=(image.float().clamp(0,1).cpu().numpy().transpose(1,2,0)*65535.0+0.5).astype('<u2')
        pixels.tofile(out)
    del a,b,image,pixels
    if torch.cuda.is_available(): torch.cuda.empty_cache()
    print('MOTIONCAM_DONE',flush=True)
)PY");
        QString python = options.rifePythonExecutable.empty()
            ? QString("python3") : QString::fromStdString(options.rifePythonExecutable);
#ifdef _WIN32
        const stdfs::path venvPython = stdfs::path(options.rifeDirectory) / "venv/Scripts/python.exe";
        const stdfs::path dotVenvPython = stdfs::path(options.rifeDirectory) / ".venv/Scripts/python.exe";
        if (options.rifePythonExecutable.empty()) {
            if (stdfs::exists(venvPython)) python = QString::fromStdString(venvPython.string());
            else if (stdfs::exists(dotVenvPython)) python = QString::fromStdString(dotVenvPython.string());
            else python = "python";
        }
#else
        const stdfs::path venvPython = stdfs::path(options.rifeDirectory) / "venv/bin/python";
        const stdfs::path dotVenvPython = stdfs::path(options.rifeDirectory) / ".venv/bin/python";
        if (options.rifePythonExecutable.empty()) {
            if (stdfs::exists(venvPython)) python = QString::fromStdString(venvPython.string());
            else if (stdfs::exists(dotVenvPython)) python = QString::fromStdString(dotVenvPython.string());
        }
#endif
        rife.setProcessChannelMode(QProcess::MergedChannels);
        rife.start(python, {"-c", code, QString::fromStdString(options.rifeDirectory)}, QIODevice::ReadWrite);
        if (!rife.waitForStarted()) throw std::runtime_error("Could not start the RIFE Python environment");
        QByteArray startupDiagnostic;
        while (!startupDiagnostic.contains("MOTIONCAM_READY")) {
            if (!rife.waitForReadyRead(250) && rife.state() == QProcess::NotRunning) {
                startupDiagnostic += rife.readAll();
                const std::string detail = startupDiagnostic.toStdString();
                spdlog::error("RIFE initialization failed: {}", detail);
                throw std::runtime_error("RIFE initialization failed:\n" + detail);
            }
            startupDiagnostic += rife.readAll();
            if (progress && !progress(0, totalWork, "Loading the RIFE model")) {
                rife.kill();
                rife.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }
        }
    }

    stdfs::create_directories(destination);
    for (size_t index = 0; index < entries.size(); ++index) {
        report("Rendering " + entries[index].name);
        const bool delayCompression = interpolate && gapMember[index];
        auto data = filesystem.materializeFile(entries[index],
            delayCompression ? false : jpegCompression);
        if (!data) throw std::runtime_error("Failed to render " + entries[index].name);
        std::vector<uint8_t> rendered(data->begin(), data->end());
        if (writeFiles || !isDng(entries[index]))
            writeBytes(outputPath(entries[index]), rendered);
        if (fileReady && isDng(entries[index]))
            finalizedDngs[index] = rendered;
        ++completed;

        // Gap placeholders are overwritten by RIFE below. Everything else is
        // immutable now and may be consumed by a streaming finalizer.
        if ((!interpolate || !syntheticMember[index]) &&
            (!jpegCompression || !gapMember[index])) {
            ready[index] = true;
            emitReady();
        }

        if (!interpolate || gapForRight[index] < 0) continue;
        const size_t gapIndex = static_cast<size_t>(gapForRight[index]);
        const auto& gap = gaps[gapIndex];
        auto leftDng = writeFiles
            ? readBytes(outputPath(entries[gap.left])) : finalizedDngs[gap.left];
        auto rightDng = writeFiles
            ? readBytes(outputPath(entries[gap.right])) : finalizedDngs[gap.right];
        auto extractRgb = [&](std::vector<uint8_t> dng, const std::string& name,
                              uint32_t& width, uint32_t& height) {
            int repeat = 0;
            std::array<uint8_t, 4> phase{};
            const bool remosaic = DNGDecoder::getCFAMetadata(dng, repeat, phase);
            if (!DNGDecoder::ensureUncompressed(dng) ||
                (remosaic && !DNGDecoder::processHigherCFA(
                    dng, repeat, phase, QuadBayerMode::Demosaic, false)))
                throw std::runtime_error("Could not demosaic RIFE input " + name);
            std::vector<uint8_t> rgb;
            if (!DNGDecoder::extractUncompressedRGB16(dng, rgb, width, height))
                throw std::runtime_error("Could not extract RGB pixels from " + name);
            return rgb;
        };
        uint32_t width = 0, height = 0, rightWidth = 0, rightHeight = 0;
        auto leftRgb = extractRgb(leftDng, entries[gap.left].name, width, height);
        auto rightRgb = extractRgb(rightDng, entries[gap.right].name, rightWidth, rightHeight);
        if (width != rightWidth || height != rightHeight)
            throw std::runtime_error("RIFE input dimensions changed within the sequence");
        const stdfs::path stageRoot(staging.path().toStdString());
        const auto leftRaw = stageRoot / "left.rgb16", rightRaw = stageRoot / "right.rgb16";
        writeBytes(leftRaw, leftRgb); writeBytes(rightRaw, rightRgb);
        std::vector<stdfs::path> outputs;
        std::ostringstream command;
        command << width << '\t' << height << '\t' << leftRaw.string() << '\t' << rightRaw.string();
        for (size_t j = 0; j < gap.frames.size(); ++j) {
            outputs.push_back(stageRoot / ("output-" + std::to_string(j) + ".rgb16"));
            command << '\t' << std::setprecision(17)
                    << static_cast<double>(j + 1) / static_cast<double>(gap.frames.size() + 1)
                    << '|' << outputs.back().string();
        }
        command << '\n';
        const QByteArray request = QByteArray::fromStdString(command.str());
        if (rife.write(request) != request.size() || !rife.waitForBytesWritten()) {
            const std::string detail = (rife.readAll() + QByteArray("\n") +
                rife.errorString().toUtf8()).toStdString();
            spdlog::error("Could not send a frame gap to RIFE: {}", detail);
            throw std::runtime_error("Could not send a frame gap to RIFE:\n" + detail);
        }
        QByteArray diagnostic;
        bool done = false;
        while (!done) {
            if (!rife.waitForReadyRead(250) && rife.state() == QProcess::NotRunning)
                throw std::runtime_error("RIFE failed: " + (diagnostic + rife.readAll()).toStdString());
            diagnostic += rife.readAll();
            done = diagnostic.contains("MOTIONCAM_DONE");
            const std::string label = "Interpolating " + std::to_string(gap.frames.size()) +
                " duplicated frame(s) with RIFE";
            if (progress && !progress(completed, totalWork, label)) {
                rife.kill();
                rife.waitForFinished();
                throw std::runtime_error("Finalization cancelled");
            }
        }
        for (size_t j = 0; j < gap.frames.size(); ++j) {
            const size_t frame = gap.frames[j];
            auto dng = writeFiles
                ? readBytes(outputPath(entries[frame])) : finalizedDngs[frame];
            int repeat = 0;
            std::array<uint8_t, 4> phase{};
            const bool remosaic = DNGDecoder::getCFAMetadata(dng, repeat, phase);
            if (!DNGDecoder::ensureUncompressed(dng) ||
                (remosaic && !DNGDecoder::processHigherCFA(
                    dng, repeat, phase, QuadBayerMode::Demosaic, false)))
                throw std::runtime_error("Could not prepare synthesized DNG " + entries[frame].name);
            const double ratio = static_cast<double>(j + 1) / static_cast<double>(gap.frames.size() + 1);
            const auto rgb = readBytes(outputs[j]);
            if (!DNGDecoder::replaceUncompressedRGB16(dng, rgb, width, height) ||
                !DNGDecoder::interpolateFrameMetadata(dng, leftDng, rightDng, ratio) ||
                (remosaic && (!DNGDecoder::processHigherCFA(
                    dng, 2, phase, QuadBayerMode::Demosaic, true) ||
                    !DNGDecoder::packUncompressedToWhiteLevel(dng))) ||
                !DNGDecoder::markSyntheticFrame(dng))
                throw std::runtime_error("Could not rebuild synthesized DNG " + entries[frame].name);
            if (writeFiles) writeBytes(outputPath(entries[frame]), dng);
            if (fileReady) finalizedDngs[frame] = dng;
            if (!jpegCompression) ready[frame] = true;
            ++completed;
            report("Rebuilt interpolated frame " + entries[frame].name);
        }
        emitReady();
        leftRgb.clear(); leftRgb.shrink_to_fit();
        rightRgb.clear(); rightRgb.shrink_to_fit();
        for (const auto& output : outputs) stdfs::remove(output);
        stdfs::remove(leftRaw); stdfs::remove(rightRaw);
        if (jpegCompression) {
            std::vector<size_t> members{gap.left};
            members.insert(members.end(), gap.frames.begin(), gap.frames.end());
            members.push_back(gap.right);
            for (const auto member : members) {
                if (lastGapForMember[member] != static_cast<int>(gapIndex)) continue;
                report("Compressing " + entries[member].name);
                compressDng(member);
                ready[member] = true;
                ++completed;
            }
            emitReady();
        }
        if (!writeFiles) {
            std::vector<size_t> releasable{gap.left};
            releasable.insert(releasable.end(), gap.frames.begin(), gap.frames.end());
            releasable.push_back(gap.right);
            for (const auto member : releasable) {
                if (lastGapForMember[member] != static_cast<int>(gapIndex)) continue;
                finalizedDngs[member].clear();
                finalizedDngs[member].shrink_to_fit();
            }
        }
    }
    if (interpolate) {
        rife.closeWriteChannel();
        if (!rife.waitForFinished(5000)) { rife.kill(); rife.waitForFinished(); }
    }
    if (progress) progress(totalWork, totalWork, "Finalization complete");
}

FrameRateInfo calculateFrameRate(const std::vector<Timestamp>& frames) {
    if (frames.size() < 2) {
        return {0,0,0,0,0,0};
    }

    double avgDuration = 0.0;
    int validFrames = 0;
    std::vector<double> durations;
    durations.reserve(frames.size() - 1);

    for (size_t i = 1; i < frames.size(); ++i) {
        double duration = static_cast<double>(frames[i] - frames[i - 1]);
        if (duration > 0) {
            avgDuration += (duration - avgDuration) / (validFrames + 1);
            durations.push_back(duration);
            validFrames++;
        }
    }

    if (validFrames == 0) {
        return {0,0,0,0,0,0};
    }

    std::sort(durations.begin(), durations.end());

    // Helper for percentile (with linear interpolation)
    auto percentile = [&](double p) -> double {
        if (durations.empty()) return 0.0;
        double pos = p * (durations.size() - 1);
        size_t idx = static_cast<size_t>(pos);
        double frac = pos - idx;
        if (idx + 1 < durations.size()) {
            return durations[idx] * (1.0 - frac) + durations[idx + 1] * frac;
        }
        return durations.back();
    };

    // Duration percentiles (in nanoseconds)
    double minDur = durations.front();
    double q1Dur   = percentile(0.25);
    double medianDur = percentile(0.5);
    double q3Dur   = percentile(0.75);
    double maxDur  = durations.back();

    // Convert durations → frame rates (FPS)
    auto toFps = [](double dur) -> float {
        return dur > 0.0 ? static_cast<float>(1e9 / dur) : 0.0f;
    };

    return {
        toFps(maxDur),  // minFrameRate (worst)
        toFps(q3Dur),   // lower quartile
        toFps(medianDur),
        toFps(q1Dur),   // upper quartile
        toFps(minDur),  // max FPS (best)
        toFps(avgDuration)
    };

    /*double medianDuration;
    size_t mid = durations.size() / 2;
    if (durations.size() % 2 == 0) {
        medianDuration = (durations[mid - 1] + durations[mid]) / 2.0;
    } else {
        medianDuration = durations[mid];
    }

    return {
        static_cast<float>(1000000000.0 / medianDuration),
        static_cast<float>(1000000000.0 / avgDuration)
    };*/
}

float determineCFRTarget(FrameRateInfo fpsInfo, const CFRTarget& cfrTarget, bool applyCFRConversion) {
    if (!applyCFRConversion) {
        if (cfrTarget.mode == CFRMode::Custom) {
            return cfrTarget.customValue;
        }
        return fpsInfo.averageFrameRate;
    }
    
    switch (cfrTarget.mode) {
        case CFRMode::Disabled:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::PreferInteger:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 24.0f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 30.0f;
            else if (fpsInfo.medianFrameRate < 49.0) return 48.0f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 60.0f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 120.0f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 120.0f;
            else return 60.0f;
            
        case CFRMode::PreferDropFrame:
            if (fpsInfo.medianFrameRate <= 23.0 || fpsInfo.medianFrameRate >= 1000.0) return fpsInfo.medianFrameRate;
            else if (fpsInfo.medianFrameRate < 24.5) return 23.976f;
            else if (fpsInfo.medianFrameRate < 26.0) return 25.0f;
            else if (fpsInfo.medianFrameRate < 33.0) return 29.97f;
            else if (fpsInfo.medianFrameRate < 49.0) return 47.952f;
            else if (fpsInfo.medianFrameRate < 52.0) return 50.0f;
            else if (fpsInfo.medianFrameRate > 56.0 && fpsInfo.medianFrameRate < 63.0) return 59.94f;
            else if (fpsInfo.medianFrameRate > 112.0 && fpsInfo.medianFrameRate < 125.0) return 119.88f;
            else if (fpsInfo.medianFrameRate > 224.0 && fpsInfo.medianFrameRate < 250.0) return 240.0f;
            else if (fpsInfo.medianFrameRate > 448.0 && fpsInfo.medianFrameRate < 500.0) return 480.0f;
            else if (fpsInfo.medianFrameRate > 896.0 && fpsInfo.medianFrameRate < 1000.0) return 960.0f;
            else if (fpsInfo.medianFrameRate >= 63.0) return 119.88f;
            else return 59.94f;
            
        case CFRMode::MedianSlowMotion:
            return fpsInfo.medianFrameRate;
            
        case CFRMode::AverageTesting:
            return fpsInfo.averageFrameRate;
            
        case CFRMode::Custom:
            return cfrTarget.customValue;
            
        default:
            return fpsInfo.medianFrameRate;
    }
}

int getFrameNumberFromTimestamp(Timestamp timestamp, Timestamp referenceTimestamp, float frameRate) {
    if (frameRate <= 0) {
        return -1;
    }

    int64_t timeDifference = timestamp - referenceTimestamp;
    if (timeDifference < 0) {
        return -1;
    }

    double nanosecondsPerFrame = 1000000000.0 / frameRate;
    return static_cast<int>(std::round(timeDifference / nanosecondsPerFrame));
}

std::string constructFrameFilename(
    const std::string& baseName,
    int frameNumber,
    int padding,
    const std::string& extension)
{
    std::ostringstream oss;
    oss << baseName;
    oss << std::setfill('0') << std::setw(padding) << frameNumber;
    
    if (!extension.empty()) {
        if (extension[0] != '.') {
            oss << '.';
        }
        oss << extension;
    }
    
    return oss.str();
}

std::string extractFilenameWithoutExtension(const std::string& fullPath) {
    boost::filesystem::path p(fullPath);
    return p.stem().string();
}

int getScaleFromOptions(FileRenderOptions options, int draftScale) {
    if (options & RENDER_OPT_DRAFT)
        return draftScale;
    return 1;
}

void syncAudio(
    Timestamp videoTimestamp,
    std::vector<AudioChunk>& audioChunks,
    int sampleRate,
    int numChannels)
{
    if (audioChunks.empty()) {
        return;
    }

    auto audioVideoDriftMs = (audioChunks[0].timestamp - videoTimestamp) * 1e-6f;
    if (std::abs(audioVideoDriftMs) > 1000) {
        spdlog::warn("Audio drift too large, not syncing audio");
        return;
    }

    AudioSampleFormat format = audioChunks[0].format;

    if (audioVideoDriftMs > 0) {
        int audioFramesToRemove = static_cast<int>(std::round(audioVideoDriftMs * sampleRate / 1000));
        int samplesToRemove = audioFramesToRemove * numChannels;

        int samplesRemoved = 0;
        auto it = audioChunks.begin();

        while (it != audioChunks.end() && samplesRemoved < samplesToRemove) {
            int remainingSamplesToRemove = samplesToRemove - samplesRemoved;

            size_t chunkSize = it->sampleCount();
            if (chunkSize <= static_cast<size_t>(remainingSamplesToRemove)) {
                samplesRemoved += chunkSize;
                it = audioChunks.erase(it);
            }
            else {
                if (format == AudioSampleFormat::Float32) {
                    it->float32Data.erase(
                        it->float32Data.begin(),
                        it->float32Data.begin() + remainingSamplesToRemove);
                } else {
                    it->int16Data.erase(
                        it->int16Data.begin(),
                        it->int16Data.begin() + remainingSamplesToRemove);
                }
                it->timestamp += static_cast<Timestamp>(remainingSamplesToRemove * 1000 / sampleRate);
                break;
            }
        }
    }
    else {
        auto silenceDuration = -audioVideoDriftMs;
        int silenceFrames = static_cast<int>(std::round(silenceDuration * sampleRate / 1000));
        int silenceSamples = silenceFrames * numChannels;

        AudioChunk silenceChunk;
        silenceChunk.timestamp = videoTimestamp;
        silenceChunk.format = format;
        if (format == AudioSampleFormat::Float32) {
            silenceChunk.float32Data.resize(silenceSamples, 0.0f);
        } else {
            silenceChunk.int16Data.resize(silenceSamples, 0);
        }

        audioChunks.insert(audioChunks.begin(), std::move(silenceChunk));

        for (auto it = audioChunks.begin() + 1; it != audioChunks.end(); ++it) {
            it->timestamp += silenceDuration;
        }
    }
}

std::string getDisplayDataType(bool sourceRgb, int cfaSize) {
    if (cfaSize > 2)
        return "Higher CFA " + std::to_string(cfaSize) + "x" + std::to_string(cfaSize);
    if (cfaSize == 2 || !sourceRgb)
        return "Bayer CFA";
    return "RGB";
}

std::string getDisplayDataLevels(
    float dynWhiteLevel, std::array<float, 4> dynBlackLevel, 
    float statWhiteLevel, std::array<float, 4> statBlackLevel, 
    std::string levels, std::string logTransform,
    bool applyShadingMap, bool normalizeShadingMap) {

    const auto resolvedLevels = resolveDataLevels(
        levels, dynWhiteLevel, dynBlackLevel, statWhiteLevel, statBlackLevel);
    float srcWhiteLevel = resolvedLevels.white;
    std::array<float, 4> srcBlackLevel = resolvedLevels.black;

    float dstWhiteLevel = srcWhiteLevel;
    std::array<float, 4> dstBlackLevel = srcBlackLevel;

    int useBits = std::min(16, static_cast<int>(std::ceil(std::log2(srcWhiteLevel + 1))));

    if(logTransform.empty()) {
        if(applyShadingMap) {
            useBits += 2;
            if(normalizeShadingMap)
                useBits += 2;
            dstWhiteLevel = std::pow(2.0f, std::min(16, useBits)) - 1;
            for (auto& v : dstBlackLevel)
                v = 0;
        }            
    } else {
        if (logTransform == "Reduce by 2bit" || logTransform == "Reduce by 2bit lq")
            useBits -= 2;
        if (logTransform == "Reduce by 4bit" || logTransform == "Reduce by 4bit lq")
            useBits -= 4;
        if (logTransform == "Reduce by 6bit" || logTransform == "Reduce by 6bit lq")
            useBits -= 6;
        if (logTransform == "Reduce by 8bit" || logTransform == "Reduce by 8bit lq")
            useBits -= 8;
        useBits = std::clamp(useBits, 1, 16);
        dstWhiteLevel = std::pow(2.0f, useBits) - 1;
        for (auto& v : dstBlackLevel)
            v = 0;
    }    
    
    std::string result = std::to_string(static_cast<int>(srcWhiteLevel)) + "/" + 
                         std::to_string(static_cast<int>(srcBlackLevel[0]));    // Build levels info string
    
    if (srcBlackLevel[0] != dstBlackLevel[0]) {
        result += " -> " + std::to_string(static_cast<int>(dstWhiteLevel)) + "/" + 
                           std::to_string(static_cast<int>(dstBlackLevel[0]));
    }       // Show transformation if levels changed
    
    result += " RAW" + std::to_string(std::min(16, useBits));
    if (!logTransform.empty()) 
        result += " log";    

    return result;
}

} // namespace vfs
} // namespace motioncam
