#pragma once

#include "anichar/color_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace anichar {

// RGB bytes after decoding, RGB conversion, and thumbnailing to at most 128x128.
struct RgbImage {
    std::size_t width = 0;
    std::size_t height = 0;
    std::vector<std::uint8_t> pixels;
};

struct HairDetection {
    Rgb rgb;
    double confidence;
};

std::optional<HairDetection> detectHair(const RgbImage& image);

} // namespace anichar
