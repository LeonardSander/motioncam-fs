#pragma once

#include <vector>
#include <string>
#include <variant>
#include <cstdint>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <functional>
#include <locale>

#include <boost/filesystem.hpp>

namespace motioncam {

constexpr float DNG_COMPRESSION_JPEG_DCT = -2.0f;
constexpr float DNG_COMPRESSION_JPEG_LOSSLESS = -1.0f;

inline bool isLossyJpegDct(float value) {
    return value <= DNG_COMPRESSION_JPEG_DCT + 0.01f;
}

typedef int64_t Timestamp;

enum EntryType : int {
    FILE_ENTRY = 0,
    DIRECTORY_ENTRY = 1,
    INVALID_ENTRY = -1
};

struct Entry {
    EntryType type;
    std::vector<std::string> pathParts;
    std::string name;
    size_t size;
    std::variant<int64_t> userData;
    bool duplicateFrame = false;
    bool syntheticFrame = false;
    // Stable identity of the original image. Unlike timestamps, this remains
    // unambiguous when damaged or repeated timestamps occur.
    int sourceFrame = -1;

    // Custom hash function for Entry
    struct Hash {
        size_t operator()(const Entry& entry) const {
            size_t hash = std::hash<int>{}(static_cast<int>(entry.type));

            // Hash the path parts
            for (const auto& part : entry.pathParts) {
                hash ^= std::hash<std::string>{}(part) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }

            // Hash the name
            hash ^= std::hash<std::string>{}(entry.name) + 0x9e3779b9 + (hash << 6) + (hash >> 2);

            return hash;
        }
    };

    // Custom equality operator for Entry
    bool operator==(const Entry& other) const {
        return type == other.type &&
               pathParts == other.pathParts &&
               name == other.name;
    }

    boost::filesystem::path getFullPath() const {
        namespace fs = boost::filesystem;

        fs::path result;

        // Add each path part
        for (const auto& part : pathParts) {
            result /= part;
        }

        // Add the filename
        result /= name;

        return result;
    }
};

enum FileRenderOptions : unsigned int {
    RENDER_OPT_NONE                         = 0,
    RENDER_OPT_DRAFT                        = 1 << 0,    
    RENDER_OPT_APPLY_VIGNETTE_CORRECTION    = 1 << 1,
    RENDER_OPT_NORMALIZE_SHADING_MAP        = 1 << 2,
    RENDER_OPT_DEBUG_SHADING_MAP            = 1 << 3,
    RENDER_OPT_VIGNETTE_ONLY_COLOR          = 1 << 4,
    RENDER_OPT_NORMALIZE_EXPOSURE           = 1 << 5,
    RENDER_OPT_FRAMERATE_CONVERSION         = 1 << 6,
    RENDER_OPT_CROPPING                     = 1 << 7,
    RENDER_OPT_CAMMODEL_OVERRIDE            = 1 << 8,
    RENDER_OPT_LOG_TRANSFORM                = 1 << 9,
    RENDER_OPT_INTERPRET_AS_QUAD_BAYER      = 1 << 10,
    RENDER_OPT_REMOSAIC_TO_BAYER            = 1 << 11,    
    RENDER_OPT_JPEG_COMPRESSION             = 1 << 12,
    RENDER_OPT_SMOOTH_EXPOSURE              = 1 << 13,
    RENDER_OPT_SMOOTH_WHITE_BALANCE         = 1 << 14,
    RENDER_OPT_HIGHER_CFA_HQ                = 1 << 15,
    RENDER_OPT_BAKE_ISO                     = 1 << 16,
    RENDER_OPT_OPTIMIZE_GAIN_MAPS            = 1 << 17
};

// Overload bitwise OR operator
inline FileRenderOptions operator|(FileRenderOptions a, FileRenderOptions b) {
    return static_cast<FileRenderOptions>(static_cast<unsigned int>(a) | static_cast<unsigned int>(b));
}

// Overload compound assignment OR operator
inline FileRenderOptions& operator|=(FileRenderOptions& a, FileRenderOptions b) {
    return a = a | b;
}

// Overload bitwise AND operator (for checking flags)
inline FileRenderOptions operator&(FileRenderOptions a, FileRenderOptions b) {
    return static_cast<FileRenderOptions>(static_cast<unsigned int>(a) & static_cast<unsigned int>(b));
}

// Overload compound assignment AND operator
inline FileRenderOptions& operator&=(FileRenderOptions& a, FileRenderOptions b) {
    return a = a & b;
}

// Overload bitwise NOT operator (for clearing flags)
inline FileRenderOptions operator~(FileRenderOptions a) {
    return static_cast<FileRenderOptions>(~static_cast<unsigned int>(a));
}

static std::string optionsToString(FileRenderOptions options) {
    if (options == RENDER_OPT_NONE) {
        return "NONE";
    }

    std::vector<std::string> flags;

    if (options & RENDER_OPT_DRAFT) {
        flags.push_back("DRAFT");
    }
    if (options & RENDER_OPT_APPLY_VIGNETTE_CORRECTION) {
        flags.push_back("VIGNETTE_CORRECTION");
    }    
    if (options & RENDER_OPT_VIGNETTE_ONLY_COLOR) {
        flags.push_back("VIGNETTE_ONLY_COLOR");
    }
    if (options & RENDER_OPT_NORMALIZE_SHADING_MAP) {
        flags.push_back("NORMALIZE_SHADING_MAP");
    }
    if (options & RENDER_OPT_DEBUG_SHADING_MAP) {
        flags.push_back("DEBUG_SHADING_MAP");
    }
    if (options & RENDER_OPT_NORMALIZE_EXPOSURE) {
        flags.push_back("NORMALIZE_EXPOSURE");
    }
    if (options & RENDER_OPT_SMOOTH_EXPOSURE) {
        flags.push_back("SMOOTH_EXPOSURE");
    }
    if (options & RENDER_OPT_SMOOTH_WHITE_BALANCE) {
        flags.push_back("SMOOTH_WHITE_BALANCE");
    }
    if (options & RENDER_OPT_HIGHER_CFA_HQ) {
        flags.push_back("HIGHER_CFA_HQ");
    }
    if (options & RENDER_OPT_BAKE_ISO) {
        flags.push_back("BAKE_ISO");
    }
    if (options & RENDER_OPT_OPTIMIZE_GAIN_MAPS) {
        flags.push_back("OPTIMIZE_GAIN_MAPS");
    }
    if (options & RENDER_OPT_FRAMERATE_CONVERSION) {
        flags.push_back("FRAMERATE_CONVERSION");
    }
    if (options & RENDER_OPT_CROPPING) {
        flags.push_back("CROPPING");
    }
    if (options & RENDER_OPT_CAMMODEL_OVERRIDE) {
        flags.push_back("CAMMODEL_OVERRIDE");
    }
    if (options & RENDER_OPT_LOG_TRANSFORM) {
        flags.push_back("LOG_TRANSFORM");
    }
    if (options & RENDER_OPT_INTERPRET_AS_QUAD_BAYER) {
        flags.push_back("INTERPRET_AS_QUAD_BAYER");
    }
    if (options & RENDER_OPT_REMOSAIC_TO_BAYER) {
        flags.push_back("REMOSAIC_TO_BAYER");
    }
    if (options & RENDER_OPT_JPEG_COMPRESSION) {
        flags.push_back("JPEG_COMPRESSION");
    }
    
    std::string result;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (i > 0) result += " | ";
        result += flags[i];
    }

    return result;
}

enum class QuadBayerMode {
    Demosaic,
    DemosaicColor,
    DemosaicOCL,
    Binning,
    Bin8x8To4x4,
    CorrectQBCFAMetadata,
    WrongCFAMetadata
};

enum class BadPixelTreatment { Bake, OpcodeOnly, Disabled };

enum class VignetteCorrectionMode { Bake, Resample, Uncropped, Exclude };

inline std::string vignetteCorrectionModeToString(VignetteCorrectionMode value) {
    if (value == VignetteCorrectionMode::Resample) return "Resample";
    if (value == VignetteCorrectionMode::Uncropped) return "Uncropped";
    if (value == VignetteCorrectionMode::Exclude) return "Exclude";
    return "Bake";
}

inline VignetteCorrectionMode stringToVignetteCorrectionMode(const std::string& value) {
    if (value == "Resample") return VignetteCorrectionMode::Resample;
    if (value == "Uncropped") return VignetteCorrectionMode::Uncropped;
    if (value == "Exclude") return VignetteCorrectionMode::Exclude;
    return VignetteCorrectionMode::Bake;
}

inline std::string badPixelTreatmentToString(BadPixelTreatment value) {
    if (value == BadPixelTreatment::OpcodeOnly) return "Opcode Only";
    if (value == BadPixelTreatment::Disabled) return "Disabled";
    return "Bake";
}

inline BadPixelTreatment stringToBadPixelTreatment(const std::string& value) {
    if (value == "Opcode Only") return BadPixelTreatment::OpcodeOnly;
    if (value == "Disabled" || value == "Disable Fully") return BadPixelTreatment::Disabled;
    return BadPixelTreatment::Bake;
}

enum class LogTransformMode {
    Disabled,
    KeepInput,
    ReduceBy2Bit,
    ReduceBy4Bit,
    ReduceBy6Bit,
    ReduceBy8Bit
};

enum class CFRMode {
    Disabled,
    PreferInteger,
    PreferDropFrame,
    MedianSlowMotion,
    AverageTesting,
    Custom
};

struct CFRTarget {
    CFRMode mode;
    float customValue; // Only used if mode == Custom

    CFRTarget() : mode(CFRMode::PreferDropFrame), customValue(0.0f) {}
    CFRTarget(CFRMode m, float val = 0.0f) : mode(m), customValue(val) {}
};

// Helper functions to convert between enums and strings
inline std::string quadBayerModeToString(QuadBayerMode mode) {
    switch(mode) {
        case QuadBayerMode::Demosaic: return "Demosaic";
        case QuadBayerMode::DemosaicColor: return "Demosaic (Color)";
        case QuadBayerMode::DemosaicOCL: return "Demosaic (OCL)";
        case QuadBayerMode::Binning: return "Binning";
        case QuadBayerMode::Bin8x8To4x4: return "Bin 8x8 to 4x4";
        case QuadBayerMode::CorrectQBCFAMetadata: return "Keep CFA";
        case QuadBayerMode::WrongCFAMetadata: return "Mislabel as 2x2";
        default: return "Demosaic";
    }
}

inline QuadBayerMode stringToQuadBayerMode(const std::string& str) {
    if (str == "Demosaic" || str == "Remosaic") return QuadBayerMode::Demosaic;
    if (str == "Demosaic (Color)" || str == "Color") return QuadBayerMode::DemosaicColor;
    if (str == "Demosaic (OCL)") return QuadBayerMode::DemosaicOCL;
    if (str == "Binning") return QuadBayerMode::Binning;
    if (str == "Bin 8x8 to 4x4") return QuadBayerMode::Bin8x8To4x4;
    if (str == "Wrong CFA Metadata" || str == "Mislabel as 2x2") return QuadBayerMode::WrongCFAMetadata;
    if (str == "Correct QBCFA Metadata" || str == "Keep CFA") return QuadBayerMode::CorrectQBCFAMetadata;
    return QuadBayerMode::Demosaic;
}

inline std::string logTransformModeToString(LogTransformMode mode) {
    switch(mode) {
        case LogTransformMode::Disabled: return "";
        case LogTransformMode::KeepInput: return "Keep Input";
        case LogTransformMode::ReduceBy2Bit: return "Reduce by 2bit";
        case LogTransformMode::ReduceBy4Bit: return "Reduce by 4bit";
        case LogTransformMode::ReduceBy6Bit: return "Reduce by 6bit";
        case LogTransformMode::ReduceBy8Bit: return "Reduce by 8bit";
        default: return "Keep Input";
    }
}

inline LogTransformMode stringToLogTransformMode(const std::string& str) {
    if (str.empty() || str == "") return LogTransformMode::Disabled;
    if (str == "Keep Input") return LogTransformMode::KeepInput;
    if (str == "Reduce by 2bit") return LogTransformMode::ReduceBy2Bit;
    if (str == "Reduce by 4bit") return LogTransformMode::ReduceBy4Bit;
    if (str == "Reduce by 6bit") return LogTransformMode::ReduceBy6Bit;
    if (str == "Reduce by 8bit") return LogTransformMode::ReduceBy8Bit;
    return LogTransformMode::KeepInput;
}

inline CFRTarget stringToCFRTarget(const std::string& str) {
    if (str.empty()) return CFRTarget(CFRMode::Disabled);
    if (str == "Prefer Integer") return CFRTarget(CFRMode::PreferInteger);
    if (str == "Prefer Drop Frame") return CFRTarget(CFRMode::PreferDropFrame);
    if (str == "Median (Slowmotion)") return CFRTarget(CFRMode::MedianSlowMotion);
    if (str == "Average (Testing)") return CFRTarget(CFRMode::AverageTesting);

    // Parse custom rates independently of the process locale. std::stof uses
    // the current C locale, so a German locale can reject the decimal point
    // used by the UI presets and silently fall back to Prefer Integer.
    try {
        std::string normalized = str;
        if (normalized.find('.') == std::string::npos)
            std::replace(normalized.begin(), normalized.end(), ',', '.');
        std::istringstream stream(normalized);
        stream.imbue(std::locale::classic());
        float value = 0.0f;
        if (!(stream >> value))
            throw std::invalid_argument("invalid frame rate");
        stream >> std::ws;
        if (stream.peek() != std::char_traits<char>::eof() ||
            !std::isfinite(value) || value <= 0.0f)
            throw std::invalid_argument("trailing characters in frame rate");
        return CFRTarget(CFRMode::Custom, value);
    } catch (...) {
        return CFRTarget(CFRMode::PreferInteger);
    }
}

inline std::string cfrTargetToString(const CFRTarget& target) {
    switch(target.mode) {
        case CFRMode::Disabled: return "";
        case CFRMode::PreferInteger: return "Prefer Integer";
        case CFRMode::PreferDropFrame: return "Prefer Drop Frame";
        case CFRMode::MedianSlowMotion: return "Median (Slowmotion)";
        case CFRMode::AverageTesting: return "Average (Testing)";
        case CFRMode::Custom: return std::to_string(target.customValue);
        default: return "Prefer Integer";
    }
}

struct RenderSettings {
    FileRenderOptions options;
    int draftScale;
    CFRTarget cfrTarget;
    std::string cropTarget;
    std::string cameraModel;
    std::string levels;
    LogTransformMode logTransform;
    std::string exposureCompensation;
    BadPixelTreatment badPixelTreatment;
    VignetteCorrectionMode vignetteCorrection;
    QuadBayerMode quadBayerOption;
    std::string cfaPhase;
    // Compression selector: -2 is 12-bit CinemaDNG JPEG DCT, -1 is JPEG 92 lossless,
    // zero is lossless JXL, and positive values are lossy JXL.
    float jxlDistance;
    // Gallery/thumbnail-only display overrides. orientation is -1 when the
    // source orientation should be retained; otherwise it is clockwise
    // degrees and must be one of 0, 90, 180, or 270.
    int orientation;
    bool ignoreForwardMat;
    // Internal export mode: write normalized, unpacked 16-bit RGB staging DNGs
    // for the Camera Native encoder. This is never persisted as a UI option.
    bool cameraNativeStaging;
    // Internal gallery mode: materialized frames are consumed as a stream and
    // are not constrained by projected-file size estimates.
    bool streamingPreview;

    // Constructor with defaults
    RenderSettings()
        : options(RENDER_OPT_NONE)
        , draftScale(1)
        , cfrTarget(CFRMode::PreferInteger)
        , cropTarget("")
        , cameraModel("Panasonic")
        , levels("Dynamic")
        , logTransform(LogTransformMode::KeepInput)
        , exposureCompensation("")
        , badPixelTreatment(BadPixelTreatment::Bake)
        , vignetteCorrection(VignetteCorrectionMode::Bake)
        , quadBayerOption(QuadBayerMode::Demosaic)
        , cfaPhase("Don't override CFA")
        , jxlDistance(-1.0f)
        , orientation(-1)
        , ignoreForwardMat(false)
        , cameraNativeStaging(false)
        , streamingPreview(false)
    {}

    // Constructor with all parameters (strings for backward compatibility)
    RenderSettings(
        FileRenderOptions opts,
        int draft,
        const std::string& cfr,
        const std::string& crop,
        const std::string& cam,
        const std::string& lvl,
        const std::string& log,
        const std::string& exp = "0ev",
        const std::string& qb = "Demosaic",
        const std::string& cfa = "Don't override CFA")
        : options(opts)
        , draftScale(draft)
        , cfrTarget(stringToCFRTarget(cfr))
        , cropTarget(crop)
        , cameraModel(cam)
        , levels(lvl)
        , logTransform(stringToLogTransformMode(log))
        , exposureCompensation(exp)
        , badPixelTreatment(BadPixelTreatment::Bake)
        , vignetteCorrection((opts & RENDER_OPT_APPLY_VIGNETTE_CORRECTION)
            ? VignetteCorrectionMode::Bake : VignetteCorrectionMode::Resample)
        , quadBayerOption(stringToQuadBayerMode(qb))
        , cfaPhase(cfa)
        , jxlDistance(-1.0f)
        , orientation(-1)
        , ignoreForwardMat(false)
        , cameraNativeStaging(false)
        , streamingPreview(false)
    {}

    // Constructor with enum types directly
    RenderSettings(
        FileRenderOptions opts,
        int draft,
        const CFRTarget& cfr,
        const std::string& crop,
        const std::string& camModel,
        const std::string& lvls,
        LogTransformMode logTrans,
        const std::string& expComp,
        QuadBayerMode quadBayer,
        const std::string& cfa = "Don't override CFA")
        : options(opts)
        , draftScale(draft)
        , cfrTarget(cfr)
        , cropTarget(crop)
        , cameraModel(camModel)
        , levels(lvls)
        , logTransform(logTrans)
        , exposureCompensation(expComp)
        , badPixelTreatment(BadPixelTreatment::Bake)
        , vignetteCorrection((opts & RENDER_OPT_APPLY_VIGNETTE_CORRECTION)
            ? VignetteCorrectionMode::Bake : VignetteCorrectionMode::Resample)
        , quadBayerOption(quadBayer)
        , cfaPhase(cfa)
        , jxlDistance(-1.0f)
        , orientation(-1)
        , ignoreForwardMat(false)
        , cameraNativeStaging(false)
        , streamingPreview(false)
    {}
};

struct FinalizeOptions {
    // Skip this many source DNG frames when writing a partial sequence.
    size_t firstDngFrame = 0;
    // Omit an output DNG from persistent finalization. The index is in the
    // complete DNG sequence.
    std::function<bool(size_t)> skipDngFrame;
    bool interpolateDuplicatedFrames = false;
    // Compare adjacent source DNG image payloads and mark matching finalized
    // frames as duplicates. This is finalization-only and does not affect mounts.
    bool detectDuplicateDngs = false;
    std::string rifeDirectory;
    // Optional interpreter override, also used by deterministic integration tests.
    std::string rifePythonExecutable;
    float jxlDistance = -1.0f;
};

struct PreviewOptions {
    size_t firstFrame = 0;
    std::function<bool(size_t)> skipFrame;
};

} // namespace
