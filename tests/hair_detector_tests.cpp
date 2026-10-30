#include "anichar/hair_detector.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace anichar;
namespace {
int checks = 0;
double maxConfidenceDifference = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
RgbImage solid(std::size_t w, std::size_t h, Rgb color) {
    RgbImage image{w, h, std::vector<std::uint8_t>(w * h * 3)};
    for (std::size_t i = 0; i < w * h; ++i)
        for (int c = 0; c < 3; ++c) image.pixels[3 * i + c] = static_cast<std::uint8_t>(color[c]);
    return image;
}
void rect(RgbImage& image, int x0, int y0, int x1, int y1, Rgb color) {
    for (int y = y0; y <= y1; ++y) for (int x = x0; x <= x1; ++x) {
        const auto i = (static_cast<std::size_t>(y) * image.width + x) * 3;
        for (int c = 0; c < 3; ++c) image.pixels[i + c] = static_cast<std::uint8_t>(color[c]);
    }
}
void expected(const RgbImage& image, Rgb rgb, double confidence, const std::string& label) {
    const auto result = detectHair(image);
    check(result.has_value(), label + " succeeds");
    if (!result) return;
    check(result->rgb == rgb, label + " RGB matches Python fixture");
    const double difference = std::abs(result->confidence - confidence);
    maxConfidenceDifference = std::max(maxConfidenceDifference, difference);
    check(difference <= 1e-12, label + " confidence matches Python fixture");
}
void absent(const RgbImage& image, const std::string& label) {
    check(!detectHair(image), label + " returns Python None-equivalent");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        QImage source(320, 160, QImage::Format_ARGB32);
        source.fill(qRgba(25, 80, 210, 255));
        QByteArray encoded;
        QBuffer buffer(&encoded);
        check(buffer.open(QIODevice::WriteOnly) && source.save(&buffer, "PNG"), "encode image fixture");
        QImage thumbnail;
        const auto decoded = decodeImage(encoded, &thumbnail);
        check(decoded.width == 128 && decoded.height == 64 && decoded.pixels.size() == 128 * 64 * 3,
              "decoder scales while preserving aspect ratio and returns packed RGB");
        check(thumbnail.width() == 200 && thumbnail.height() == 100,
              "decoder also creates Python-sized card thumbnail from same decoded image");
        check(decoded.pixels[0] == 25 && decoded.pixels[1] == 80 && decoded.pixels[2] == 210,
              "decoder converts image channels to RGB");
        bool corruptImage = false;
        try { (void)decodeImage(QByteArray("not an image")); }
        catch (const std::runtime_error&) { corruptImage = true; }
        check(corruptImage, "invalid image bytes return a decoder error");

        auto dark = solid(64, 64, {240, 220, 200});
        rect(dark, 8, 0, 55, 30, {18, 15, 20});
        rect(dark, 15, 28, 50, 46, {220, 180, 150});
        expected(dark, {18, 15, 20}, .9573696079911607, "dark hair");

        auto light = solid(64, 64, {245, 245, 245});
        rect(light, 8, 0, 55, 30, {238, 205, 105});
        rect(light, 16, 28, 48, 45, {230, 190, 160});
        expected(light, {238, 205, 105}, .9074687915006558, "light blonde hair");

        auto blue = solid(64, 64, {235, 235, 235});
        rect(blue, 8, 0, 55, 30, {30, 80, 220});
        rect(blue, 16, 28, 48, 45, {225, 190, 160});
        expected(blue, {30, 80, 220}, .907468791500653, "colored blue hair");

        auto mixed = solid(64, 64, {240, 240, 240});
        rect(mixed, 8, 0, 30, 30, {220, 40, 50});
        rect(mixed, 31, 0, 55, 30, {30, 70, 210});
        rect(mixed, 16, 28, 48, 45, {220, 180, 150});
        expected(mixed, {30, 70, 210}, .7594907829895932, "ambiguous mixed colors");

        auto shadow = solid(64, 64, {240, 240, 240});
        rect(shadow, 8, 0, 55, 30, {12, 12, 16});
        rect(shadow, 8, 0, 30, 30, {30, 80, 220});
        rect(shadow, 16, 28, 48, 45, {220, 180, 150});
        expected(shadow, {30, 80, 220}, .6294328344588392, "chromatic hair demoted from dark-shadow winner");

        absent(solid(64, 64, {255, 255, 255}), "plain image/background masking");
        auto tiny = solid(16, 16, {240, 220, 200});
        rect(tiny, 0, 0, 15, 15, {10, 10, 10});
        absent(tiny, "dimensions below 20 pixels");
        auto lowPixels = solid(20, 20, {240, 220, 200});
        rect(lowPixels, 8, 0, 18, 8, {10, 10, 10});
        absent(lowPixels, "insufficient usable region pixels");

        auto atThreshold = solid(40, 40, {255, 255, 255});
        rect(atThreshold, 5, 5, 24, 14, {0, 0, 0}); // exactly 200 kept pixels
        expected(atThreshold, {0, 0, 0}, .9932367149758454, "200-pixel inclusion boundary");
        auto belowThreshold = solid(40, 40, {255, 255, 255});
        rect(belowThreshold, 5, 5, 23, 14, {0, 0, 0});
        rect(belowThreshold, 5, 15, 13, 15, {0, 0, 0}); // exactly 199 kept pixels
        absent(belowThreshold, "199-pixel exclusion boundary");

        bool malformed = false;
        try { (void)detectHair(RgbImage{20, 20, {}}); }
        catch (const std::invalid_argument&) { malformed = true; }
        check(malformed, "mismatched decoded RGB storage is rejected");
        bool unnormalized = false;
        try { (void)detectHair(solid(129, 20, {0, 0, 0})); }
        catch (const std::invalid_argument&) { unnormalized = true; }
        check(unnormalized, "input larger than Python's thumbnail bound is rejected");

        std::cout << "Passed " << checks << " Python-parity detector checks; max confidence delta "
                  << maxConfidenceDifference << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
