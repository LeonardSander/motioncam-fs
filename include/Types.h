#pragma once

#include <vector>
#include <string>
#include <variant>

#include <boost/filesystem.hpp>

namespace motioncam {

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
    
    std::string result;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (i > 0) result += " | ";
        result += flags[i];
    }

    return result;
}

enum class QuadBayerMode {
    Remosaic,
    WrongCFAMetadata,
    CorrectQBCFAMetadata
};

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
        case QuadBayerMode::Remosaic: return "Remosaic";
        case QuadBayerMode::WrongCFAMetadata: return "Wrong CFA Metadata";
        case QuadBayerMode::CorrectQBCFAMetadata: return "Correct QBCFA Metadata";
        default: return "Remosaic";
    }
}

inline QuadBayerMode stringToQuadBayerMode(const std::string& str) {
    if (str == "Remosaic") return QuadBayerMode::Remosaic;
    if (str == "Wrong CFA Metadata") return QuadBayerMode::WrongCFAMetadata;
    if (str == "Correct QBCFA Metadata") return QuadBayerMode::CorrectQBCFAMetadata;
    return QuadBayerMode::Remosaic;
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

    // Try to parse as custom float
    try {
        float value = std::stof(str);
        return CFRTarget(CFRMode::Custom, value);
    } catch (...) {
        return CFRTarget(CFRMode::PreferDropFrame);
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
        default: return "Prefer Drop Frame";
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
    QuadBayerMode quadBayerOption;

    // Constructor with defaults
    RenderSettings()
        : options(RENDER_OPT_NONE)
        , draftScale(1)
        , cfrTarget(CFRMode::PreferDropFrame)
        , cropTarget("")
        , cameraModel("Panasonic")
        , levels("Dynamic")
        , logTransform(LogTransformMode::KeepInput)
        , exposureCompensation("0ev")
        , quadBayerOption(QuadBayerMode::Remosaic)
    {}

    // Constructor with all parameters (strings for backward compatibility)
    RenderSettings(
        FileRenderOptions opts,
        int draft,
        const std::string& cfr,
        const std::string& crop,
        const std::string& camModel,
        const std::string& lvls,
        const std::string& logTrans,
        const std::string& expComp,
        const std::string& quadBayer
    )
        : options(opts)
        , draftScale(draft)
        , cfrTarget(stringToCFRTarget(cfr))
        , cropTarget(crop)
        , cameraModel(camModel)
        , levels(lvls)
        , logTransform(stringToLogTransformMode(logTrans))
        , exposureCompensation(expComp)
        , quadBayerOption(stringToQuadBayerMode(quadBayer))
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
        QuadBayerMode quadBayer
    )
        : options(opts)
        , draftScale(draft)
        , cfrTarget(cfr)
        , cropTarget(crop)
        , cameraModel(camModel)
        , levels(lvls)
        , logTransform(logTrans)
        , exposureCompensation(expComp)
        , quadBayerOption(quadBayer)
    {}
};

} // namespace
