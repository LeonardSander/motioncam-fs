#pragma once

#include "Types.h"
#include <string>

namespace motioncam {

/**
 * Consolidated configuration for file rendering options.
 * This structure encapsulates all rendering parameters that were previously
 * passed as individual arguments through multiple function calls.
 */
struct RenderConfig {
    // Bitfield options
    FileRenderOptions options = RENDER_OPT_NONE;
    
    // Draft mode settings
    int draftScale = 1;
    
    // Frame rate conversion
    std::string cfrTarget = "Prefer Drop Frame";
    
    // Cropping
    std::string cropTarget = "";
    
    // Camera model override
    std::string cameraModel = "Panasonic";
    
    // Levels adjustment
    std::string levels = "Dynamic";
    
    // Log transform
    std::string logTransform = "Keep Input";
    
    // Exposure compensation
    std::string exposureCompensation = "0ev";
    
    // Quad Bayer processing
    std::string quadBayerOption = "Remosaic";
    
    // CFA phase override
    std::string cfaPhase = "bggr";
    
    // Default constructor
    RenderConfig() = default;
    
    // Constructor with all parameters for backward compatibility
    RenderConfig(
        FileRenderOptions opts,
        int draft,
        const std::string& cfr,
        const std::string& crop,
        const std::string& cam,
        const std::string& lvl,
        const std::string& log,
        const std::string& exp = "0ev",
        const std::string& qb = "Remosaic",
        const std::string& cfa = "bggr")
        : options(opts)
        , draftScale(draft)
        , cfrTarget(cfr)
        , cropTarget(crop)
        , cameraModel(cam)
        , levels(lvl)
        , logTransform(log)
        , exposureCompensation(exp)
        , quadBayerOption(qb)
        , cfaPhase(cfa)
    {}
};

} // namespace motioncam
