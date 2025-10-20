#include "ExposureKeyframes.h"
#include <sstream>
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

namespace motioncam {

std::optional<ExposureKeyframes> ExposureKeyframes::parse(const std::string& input) {
    if (input.empty()) {
        return std::nullopt;
    }
    
    ExposureKeyframes result;
    std::istringstream stream(input);
    std::string pair;
    
    // Parse comma-separated pairs
    while (std::getline(stream, pair, ',')) {
        // Trim whitespace
        size_t start = pair.find_first_not_of(" \t\n\r");
        size_t end = pair.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) continue;
        pair = pair.substr(start, end - start + 1);
        
        // Find colon separator
        size_t colonPos = pair.find(':');
        if (colonPos == std::string::npos) {
            spdlog::warn("Invalid keyframe pair format: {}", pair);
            continue;
        }
        
        std::string posStr = pair.substr(0, colonPos);
        std::string valStr = pair.substr(colonPos + 1);
        
        // Trim position and value strings
        start = posStr.find_first_not_of(" \t\n\r");
        end = posStr.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            posStr = posStr.substr(start, end - start + 1);
        }
        
        start = valStr.find_first_not_of(" \t\n\r");
        end = valStr.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            valStr = valStr.substr(start, end - start + 1);
        }
        
        // Parse position (handle "start" and "end" synonyms)
        float position;
        if (posStr == "start") {
            position = 0.0f;
        } else if (posStr == "end") {
            position = 1.0f;
        } else {
            try {
                position = std::stof(posStr);
            } catch (const std::exception& e) {
                spdlog::warn("Invalid position value: {}", posStr);
                continue;
            }
        }
        
        // Validate position range
        if (position < 0.0f || position > 1.0f) {
            spdlog::warn("Position out of range [0,1]: {}", position);
            continue;
        }
        
        // Parse value
        float value;
        try {
            value = std::stof(valStr);
        } catch (const std::exception& e) {
            spdlog::warn("Invalid exposure value: {}", valStr);
            continue;
        }
        
        result.mKeyframes.emplace_back(position, value);
    }
    
    if (result.mKeyframes.empty()) {
        return std::nullopt;
    }
    
    // Sort keyframes by position
    std::sort(result.mKeyframes.begin(), result.mKeyframes.end());
    
    // Calculate derivatives based on rules:
    // - derivative = 0 for most keyframes (smooth)
    // - derivative != 0 only at:
    //   1. position 0 or 1 (start/end)
    //   2. keyframes that follow a smaller value and are followed by a bigger value (strictly increasing)
    //   3. keyframes that follow a bigger value and are followed by a smaller value (strictly decreasing)
    for (size_t i = 0; i < result.mKeyframes.size(); ++i) {
        auto& kf = result.mKeyframes[i];
        
        // Allow non-zero derivative at start (position 0)
        if (kf.position == 0.0f && i + 1 < result.mKeyframes.size()) {
            // Calculate slope to next keyframe
            float slope = (result.mKeyframes[i + 1].value - kf.value) / 
                         (result.mKeyframes[i + 1].position - kf.position);
            kf.derivative = slope;
            continue;
        }
        
        // Allow non-zero derivative at end (position 1)
        if (kf.position == 1.0f && i > 0) {
            // Calculate slope from previous keyframe
            float slope = (kf.value - result.mKeyframes[i - 1].value) / 
                         (kf.position - result.mKeyframes[i - 1].position);
            kf.derivative = slope;
            continue;
        }
        
        // Check if this is a monotonic point (strictly increasing or decreasing)
        if (i > 0 && i + 1 < result.mKeyframes.size()) {
            float prevValue = result.mKeyframes[i - 1].value;
            float nextValue = result.mKeyframes[i + 1].value;
            
            // Strictly increasing: prev < current < next
            bool isStrictlyIncreasing = prevValue < kf.value && kf.value < nextValue;
            // Strictly decreasing: prev > current > next
            bool isStrictlyDecreasing = prevValue > kf.value && kf.value > nextValue;
            
            if (isStrictlyIncreasing || isStrictlyDecreasing) {
                // At monotonic points, allow non-zero derivative (average slope)
                float slopeBefore = (kf.value - prevValue) / (kf.position - result.mKeyframes[i - 1].position);
                float slopeAfter = (nextValue - kf.value) / (result.mKeyframes[i + 1].position - kf.position);
                kf.derivative = (slopeBefore + slopeAfter) * 0.5f;
            } else {
                // At extrema or other points, derivative = 0 for smooth transition
                kf.derivative = 0.0f;
            }
        } else {
            // Default to 0 derivative
            kf.derivative = 0.0f;
        }
    }
    
    spdlog::info("Parsed {} exposure keyframes", result.mKeyframes.size());
    for (const auto& kf : result.mKeyframes) {
        spdlog::debug("  Keyframe: pos={:.3f}, value={:.2f}ev, deriv={:.2f}", 
                     kf.position, kf.value, kf.derivative);
    }
    
    return result;
}

float ExposureKeyframes::interpolate(float t, const ExposureKeyframe& k0, const ExposureKeyframe& k1) const {
    // Cubic Hermite spline interpolation
    // h(t) = (2t³ - 3t² + 1)p0 + (t³ - 2t² + t)m0 + (-2t³ + 3t²)p1 + (t³ - t²)m1
    
    float t2 = t * t;
    float t3 = t2 * t;
    
    float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    float h10 = t3 - 2.0f * t2 + t;
    float h01 = -2.0f * t3 + 3.0f * t2;
    float h11 = t3 - t2;
    
    // Scale derivatives by the interval length
    float interval = k1.position - k0.position;
    float m0 = k0.derivative * interval;
    float m1 = k1.derivative * interval;
    
    return h00 * k0.value + h10 * m0 + h01 * k1.value + h11 * m1;
}

float ExposureKeyframes::getExposureAt(float normalizedPosition) const {
    if (mKeyframes.empty()) {
        return 0.0f;
    }
    
    // Clamp position to [0, 1]
    normalizedPosition = std::max(0.0f, std::min(1.0f, normalizedPosition));
    
    // Single keyframe - return constant value
    if (mKeyframes.size() == 1) {
        return mKeyframes[0].value;
    }
    
    // Before first keyframe - return first value
    if (normalizedPosition <= mKeyframes[0].position) {
        return mKeyframes[0].value;
    }
    
    // After last keyframe - return last value
    if (normalizedPosition >= mKeyframes.back().position) {
        return mKeyframes.back().value;
    }
    
    // Find the two keyframes to interpolate between
    for (size_t i = 0; i < mKeyframes.size() - 1; ++i) {
        const auto& k0 = mKeyframes[i];
        const auto& k1 = mKeyframes[i + 1];
        
        if (normalizedPosition >= k0.position && normalizedPosition <= k1.position) {
            // Calculate normalized t within this segment
            float t = (normalizedPosition - k0.position) / (k1.position - k0.position);
            return interpolate(t, k0, k1);
        }
    }
    
    // Fallback (should not reach here)
    return mKeyframes.back().value;
}

float ExposureKeyframes::getExposureAtFrame(int frameIndex, int totalFrames) const {
    if (totalFrames <= 1) {
        return getExposureAt(0.0f);
    }
    
    // Calculate normalized position
    // frameIndex 0 -> position 0.0
    // frameIndex (totalFrames-1) -> position 1.0
    float normalizedPosition = static_cast<float>(frameIndex) / static_cast<float>(totalFrames - 1);
    
    return getExposureAt(normalizedPosition);
}

} // namespace motioncam
