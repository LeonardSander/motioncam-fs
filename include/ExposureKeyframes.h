#ifndef EXPOSUREKEYFRAMES_H
#define EXPOSUREKEYFRAMES_H

#include <string>
#include <vector>
#include <map>
#include <optional>

namespace motioncam {

struct ExposureKeyframe {
    float position;  // 0.0 to 1.0 (normalized position in sequence)
    float value;     // exposure value in EV
    float derivative; // derivative at this point (0 for smooth, non-zero for sharp transitions)
    
    ExposureKeyframe(float pos, float val, float deriv = 0.0f)
        : position(pos), value(val), derivative(deriv) {}
    
    bool operator<(const ExposureKeyframe& other) const {
        return position < other.position;
    }
};

class ExposureKeyframes {
public:
    // Parse keyframe string like "0.2:-4, 0.4:2.4" or "start:-2, 0.5:0, end:2"
    static std::optional<ExposureKeyframes> parse(const std::string& input);
    
    // Get exposure value at normalized position (0.0 to 1.0)
    float getExposureAt(float normalizedPosition) const;
    
    // Get exposure value at specific frame
    float getExposureAtFrame(int frameIndex, int totalFrames) const;
    
    // Check if keyframes are valid
    bool isValid() const { return !mKeyframes.empty(); }
    
    // Get raw keyframes
    const std::vector<ExposureKeyframe>& getKeyframes() const { return mKeyframes; }
    
private:
    ExposureKeyframes() = default;
    
    // Cubic Hermite spline interpolation
    float interpolate(float t, const ExposureKeyframe& k0, const ExposureKeyframe& k1) const;
    
    std::vector<ExposureKeyframe> mKeyframes;
};

} // namespace motioncam

#endif // EXPOSUREKEYFRAMES_H
