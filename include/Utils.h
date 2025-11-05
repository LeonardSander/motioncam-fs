#pragma once


#include <iostream>
#include <vector>
#include <streambuf>
#include <ostream>
#include <algorithm>
#include <memory>

#include "Types.h"

namespace motioncam {

struct CameraFrameMetadata;
struct CameraConfiguration;

namespace utils {

class vectorbuf : public std::streambuf {
private:
    std::vector<char>& vec_;
    friend class vector_ostream;

public:
    explicit vectorbuf(std::vector<char>& vec) : vec_(vec) {
        // Set up the put area to use the vector's data
        if (!vec_.empty()) {
            setp(vec_.data(), vec_.data() + vec_.size());
        }
    }

protected:
    // Called when the put area is full
    virtual int_type overflow(int_type c) override {
        if (c != traits_type::eof()) {
            // Expand the vector
            size_t old_size = vec_.size();
            vec_.resize(old_size + 1);
            vec_[old_size] = static_cast<char>(c);

            // Update put pointers
            setp(vec_.data(), vec_.data() + vec_.size());
            pbump(static_cast<int>(old_size + 1));
        }
        return c;
    }

    // Called when data needs to be written to the underlying storage
    virtual std::streamsize xsputn(const char* s, std::streamsize count) override {
        size_t old_size = vec_.size();
        size_t available = epptr() - pptr();

        if (static_cast<size_t>(count) > available) {
            // Need to expand the vector
            vec_.resize(old_size + count);
            setp(vec_.data(), vec_.data() + vec_.size());
            pbump(static_cast<int>(old_size));
        }

        std::copy(s, s + count, pptr());
        pbump(static_cast<int>(count));

        return count;
    }

    // For random access positioning
    virtual pos_type seekoff(off_type off, std::ios_base::seekdir way,
                             std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override {
        if (which & std::ios_base::out) {
            pos_type pos;

            switch (way) {
            case std::ios_base::beg:
                pos = off;
                break;
            case std::ios_base::cur:
                pos = (pptr() - pbase()) + off;
                break;
            case std::ios_base::end:
                pos = vec_.size() + off;
                break;
            default:
                return pos_type(off_type(-1));
            }

            return seekpos(pos, which);
        }

        return pos_type(off_type(-1));
    }

    virtual pos_type seekpos(pos_type sp, std::ios_base::openmode which = std::ios_base::in | std::ios_base::out) override {
        if (which & std::ios_base::out) {
            off_type pos = sp;

            if (pos < 0) {
                return pos_type(off_type(-1));
            }

            // Ensure vector is large enough
            if (static_cast<size_t>(pos) > vec_.size()) {
                vec_.resize(static_cast<size_t>(pos));
            }

            // Update put pointers
            setp(vec_.data(), vec_.data() + vec_.size());
            pbump(static_cast<int>(pos));

            return sp;
        }

        return pos_type(off_type(-1));
    }
};

class vector_ostream : public std::ostream {
private:
    vectorbuf buf_;

public:
    explicit vector_ostream(std::vector<char>& vec)
        : std::ostream(&buf_), buf_(vec) {}

    // Get reference to the underlying vector
    std::vector<char>& vector() {
        return buf_.vec_;
    }

    const std::vector<char>& vector() const {
        return buf_.vec_;
    }

    // Get current write position
    std::streampos tell() {
        return tellp();
    }

    // Seek to position from beginning
    vector_ostream& seek(std::streampos pos) {
        seekp(pos);
        return *this;
    }

    // Seek relative to current position
    vector_ostream& seek_relative(std::streamoff off) {
        seekp(off, std::ios_base::cur);
        return *this;
    }

    // Seek from end
    vector_ostream& seek_from_end(std::streamoff off) {
        seekp(off, std::ios_base::end);
        return *this;
    }
};

std::shared_ptr<std::vector<char>> generateDng(
    std::vector<uint8_t>& data,
    const CameraFrameMetadata& metadata,
    const CameraConfiguration& cameraConfiguration,
    float recordingFps,
    int frameNumber,
    double baselineExpValue,
    const RenderSettings& settings
);

std::pair<int, int> toFraction(float frameRate, int base = 1000);

} // namespace utils
} // namespace motioncam
