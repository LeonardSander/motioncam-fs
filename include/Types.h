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
        flags.push_back("DRAFT");    }
    
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
    
    std::string result;
    for (size_t i = 0; i < flags.size(); ++i) {
        if (i > 0) result += " | ";
        result += flags[i];
    }

    return result;
}

} // namespace
