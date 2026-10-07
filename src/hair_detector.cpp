#include "anichar/hair_detector.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace anichar {
namespace {
using Vec3 = std::array<double, 3>;

struct Pixel {
    Lab lab;
    Rgb rgb;
    double y;
};

Vec3 channels(const Lab& v) { return {v.l, v.a, v.b}; }

double median(std::vector<double> values) {
    const auto middle = values.begin() + values.size() / 2;
    std::nth_element(values.begin(), middle, values.end());
    const double upper = *middle;
    if (values.size() % 2) return upper;
    return (upper + *std::max_element(values.begin(), middle)) / 2.0;
}

Vec3 median3(const std::vector<Vec3>& values) {
    Vec3 out{};
    for (int c = 0; c < 3; ++c) {
        std::vector<double> channel;
        channel.reserve(values.size());
        for (const auto& v : values) channel.push_back(v[c]);
        out[c] = median(std::move(channel));
    }
    return out;
}

double stddev(const std::vector<Vec3>& values, int channel) {
    double mean = 0;
    for (const auto& v : values) mean += v[channel];
    mean /= values.size();
    double sum = 0;
    for (const auto& v : values) {
        const double d = v[channel] - mean;
        sum += d * d;
    }
    return std::sqrt(sum / values.size());
}

Vec3 hsv(const Vec3& rgb) {
    const double r = rgb[0] / 255.0, g = rgb[1] / 255.0, b = rgb[2] / 255.0;
    const double high = std::max({r, g, b}), low = std::min({r, g, b});
    const double delta = high - low;
    double h = 0, s = 0;
    if (high != 0) s = delta / high;
    if (low != high) {
        const double rc = (high - r) / delta, gc = (high - g) / delta, bc = (high - b) / delta;
        if (r == high) h = bc - gc;
        else if (g == high) h = 2.0 + rc - bc;
        else h = 4.0 + gc - rc;
        h = std::fmod(h / 6.0, 1.0);
        if (h < 0) h += 1.0;
    }
    return {h, s, high};
}

std::string family(const Vec3& rgb) {
    const auto h = hsv(rgb);
    const double hue = h[0] * 360;
    if (h[2] < .22) return "black";
    if (h[1] < .14) return h[2] > .82 ? "white" : "gray";
    if (hue < 15 || hue >= 345) return h[1] < .55 && h[2] > .7 ? "pink" : "red";
    if (hue < 45) return h[2] < .6 && h[1] > .3 ? "brown" : "orange";
    if (hue < 68) return "yellow";
    if (hue < 165) return "green";
    if (hue < 190) return "cyan";
    if (hue < 255) return "blue";
    if (hue < 295) return "purple";
    return "pink";
}

bool skinlike(const Vec3& rgb) {
    const auto h = hsv(rgb);
    return h[0] < .12 && .15 < h[1] && h[1] < .6 && h[2] > .55;
}

class NumpySeedZero {
public:
    std::uint32_t next32() {
        if (has32_) {
            has32_ = false;
            return cached32_;
        }
        const std::uint64_t value = next64();
        cached32_ = static_cast<std::uint32_t>(value >> 32);
        has32_ = true;
        return static_cast<std::uint32_t>(value);
    }

    std::uint64_t bounded(std::uint64_t max) {
        if (max == 0) return 0;
        const std::uint64_t range = max + 1;
        const std::uint64_t threshold = (std::numeric_limits<std::uint32_t>::max() - max) % range;
        for (;;) {
            const std::uint64_t product = static_cast<std::uint64_t>(next32()) * range;
            const auto low = static_cast<std::uint32_t>(product);
            if (low >= range || low >= threshold) return product >> 32;
        }
    }

private:
    // NumPy 2.5.3 default_rng(0) PCG64 state and increment; choice() uses Floyd sampling plus shuffle.
    using U128 = unsigned __int128;
    U128 state_ = (static_cast<U128>(0x1aa1b5345996452dULL) << 64) | 0x09585eb7a69561e3ULL;
    static constexpr U128 increment_ = (static_cast<U128>(0x418ddadb3af71a82ULL) << 64) | 0x588133bc447873a9ULL;
    static constexpr U128 multiplier_ = (static_cast<U128>(0x2360ed051fc65da4ULL) << 64) | 0x4385df649fccf645ULL;
    bool has32_ = false;
    std::uint32_t cached32_ = 0;

    std::uint64_t next64() {
        state_ = state_ * multiplier_ + increment_;
        const auto mixed = static_cast<std::uint64_t>(state_ >> 64) ^ static_cast<std::uint64_t>(state_);
        const unsigned rotation = static_cast<unsigned>(state_ >> 122);
        return (mixed >> rotation) | (mixed << ((64 - rotation) & 63));
    }
};

std::vector<std::size_t> numpyChoiceFive(std::size_t population) {
    NumpySeedZero rng;
    std::array<std::uint64_t, 8> set;
    set.fill(std::numeric_limits<std::uint64_t>::max());
    std::uint64_t mask = 6; // int(1.2 * 5), expanded by NumPy _gen_mask
    mask |= mask >> 1; mask |= mask >> 2; mask |= mask >> 4;
    mask |= mask >> 8; mask |= mask >> 16; mask |= mask >> 32;
    std::vector<std::size_t> result(5);
    for (std::size_t j = population - 5; j < population; ++j) {
        std::uint64_t value = rng.bounded(j);
        std::size_t slot = static_cast<std::size_t>(value & mask);
        while (set[slot] != std::numeric_limits<std::uint64_t>::max() && set[slot] != value)
            slot = (slot + 1) & mask;
        if (set[slot] == std::numeric_limits<std::uint64_t>::max()) {
            set[slot] = value;
            result[j - population + 5] = static_cast<std::size_t>(value);
        } else {
            slot = static_cast<std::size_t>(j & mask);
            while (set[slot] != std::numeric_limits<std::uint64_t>::max()) slot = (slot + 1) & mask;
            set[slot] = j;
            result[j - population + 5] = j;
        }
    }
    for (std::size_t i = 4; i > 0; --i) {
        const auto j = static_cast<std::size_t>(rng.bounded(i));
        std::swap(result[i], result[j]);
    }
    return result;
}

std::vector<int> kmeans(const std::vector<Pixel>& points) {
    std::array<Vec3, 5> centers{};
    const auto initial = numpyChoiceFive(points.size());
    for (std::size_t i = 0; i < centers.size(); ++i) centers[i] = channels(points[initial[i]].lab);
    std::vector<int> labels(points.size());
    for (int iteration = 0; iteration < 12; ++iteration) {
        std::array<Vec3, 5> sums{};
        std::array<std::size_t, 5> counts{};
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto p = channels(points[i].lab);
            int winner = 0;
            double best = std::numeric_limits<double>::infinity();
            for (int c = 0; c < 5; ++c) {
                const double l = p[0] - centers[c][0], a = p[1] - centers[c][1], b = p[2] - centers[c][2];
                const double distance = l * l + a * a + b * b;
                if (distance < best) { best = distance; winner = c; }
            }
            labels[i] = winner;
            ++counts[winner];
            for (int c = 0; c < 3; ++c) sums[winner][c] += p[c];
        }
        for (int c = 0; c < 5; ++c)
            if (counts[c]) for (int axis = 0; axis < 3; ++axis) centers[c][axis] = sums[c][axis] / counts[c];
    }
    return labels;
}

struct Group {
    std::string color;
    double score = 0;
    std::vector<std::size_t> points;
};

Vec3 medianRgb(const std::vector<Pixel>& points, const std::vector<std::size_t>& members) {
    std::vector<Vec3> values;
    values.reserve(members.size());
    for (auto i : members) values.push_back({double(points[i].rgb[0]), double(points[i].rgb[1]), double(points[i].rgb[2])});
    return median3(values);
}

Vec3 medianLab(const std::vector<Pixel>& points, const std::vector<std::size_t>& members) {
    std::vector<Vec3> values;
    values.reserve(members.size());
    for (auto i : members) values.push_back(channels(points[i].lab));
    return median3(values);
}
}

std::optional<HairDetection> detectHair(const RgbImage& image) {
    if (image.width == 0 || image.height == 0 || image.width > 128 || image.height > 128 ||
        image.width > std::numeric_limits<std::size_t>::max() / image.height / 3 ||
        image.pixels.size() != image.width * image.height * 3)
        throw std::invalid_argument("Expected decoded RGB bytes after thumbnailing to 128x128");
    const auto w = image.width, h = image.height;
    if (h < 20 || w < 20) return std::nullopt;

    std::vector<Lab> labs(w * h);
    auto pixelRgb = [&](std::size_t i) {
        return Rgb{image.pixels[3 * i], image.pixels[3 * i + 1], image.pixels[3 * i + 2]};
    };
    for (std::size_t i = 0; i < labs.size(); ++i) labs[i] = toLab(pixelRgb(i));

    std::vector<Vec3> border;
    border.reserve(6 * w + 6 * h);
    auto addBorder = [&](std::size_t x, std::size_t y) { border.push_back(channels(labs[y * w + x])); };
    for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < w; ++x) addBorder(x, y);
    for (std::size_t y = h - 3; y < h; ++y) for (std::size_t x = 0; x < w; ++x) addBorder(x, y);
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = 0; x < 3; ++x) addBorder(x, y);
    for (std::size_t y = 0; y < h; ++y) for (std::size_t x = w - 3; x < w; ++x) addBorder(x, y);
    Vec3 borderStd{};
    for (int c = 0; c < 3; ++c) borderStd[c] = stddev(border, c);
    const bool maskBackground = std::max({borderStd[0], borderStd[1], borderStd[2]}) < 12;
    Vec3 borderMedian{};
    if (maskBackground) borderMedian = median3(border);

    const auto regionHeight = static_cast<std::size_t>(h * .55);
    std::vector<Pixel> points;
    std::size_t regionCount = 0;
    for (std::size_t y = 0; y < regionHeight; ++y) {
        for (std::size_t x = 0; x < w; ++x) {
            const bool inRegion = x >= static_cast<std::size_t>(w * .07) &&
                                  x < static_cast<std::size_t>(w * .93) &&
                                  !((double(y) / h > .42) && (std::abs(double(x) / w - .5) < .2));
            if (!inRegion) continue;
            ++regionCount;
            const auto i = y * w + x;
            const auto lab = channels(labs[i]);
            if (maskBackground) {
                const double dl = lab[0] - borderMedian[0], da = lab[1] - borderMedian[1], db = lab[2] - borderMedian[2];
                if (std::sqrt(dl * dl + da * da + db * db) < 10) continue;
            }
            points.push_back({labs[i], pixelRgb(i), double(y) / regionHeight});
        }
    }
    if (points.size() < 200) return std::nullopt;

    const auto labels = kmeans(points);
    std::vector<Group> groups;
    for (int cluster = 0; cluster < 5; ++cluster) {
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < labels.size(); ++i) if (labels[i] == cluster) members.push_back(i);
        const double share = double(members.size()) / points.size();
        if (share < .03) continue;
        const auto rgb = medianRgb(points, members);
        double yMean = 0;
        for (auto i : members) yMean += points[i].y;
        yMean /= members.size();
        const double score = share * (1 - .5 * yMean) * (skinlike(rgb) ? .4 : 1.0);
        const auto color = family(rgb);
        auto found = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.color == color; });
        if (found == groups.end()) groups.push_back({color, score, std::move(members)});
        else {
            found->score += score;
            found->points.insert(found->points.end(), members.begin(), members.end());
        }
    }
    if (groups.empty()) return std::nullopt;
    std::stable_sort(groups.begin(), groups.end(), [](const Group& a, const Group& b) { return a.score > b.score; });
    const Group* winner = &groups.front();
    bool demoted = false;
    if (winner->color == "black" || winner->color == "gray") {
        for (std::size_t i = 1; i < groups.size(); ++i) {
            const auto& candidate = groups[i];
            if (candidate.color != "black" && candidate.color != "gray" && candidate.color != "white" &&
                candidate.score >= .5 * winner->score && !skinlike(medianRgb(points, candidate.points))) {
                winner = &candidate;
                demoted = true;
                break;
            }
        }
    }

    double second = 0;
    for (const auto& group : groups) if (group.color != winner->color) second = std::max(second, group.score);
    const double dominance = winner->score / (winner->score + second);
    std::vector<Vec3> selectedLab;
    selectedLab.reserve(winner->points.size());
    for (auto i : winner->points) selectedLab.push_back(channels(points[i].lab));
    const double labStd = (stddev(selectedLab, 0) + stddev(selectedLab, 1) + stddev(selectedLab, 2)) / 3;
    const double tightness = std::max(0.0, 1 - labStd / 35);
    const double coverage = std::min(1.0, double(points.size()) / (regionCount * .3));
    const double confidence = std::min(1.0, (.5 * dominance + .3 * tightness + .2 * coverage) * (demoted ? .85 : 1.0));
    const auto rgb = medianRgb(points, winner->points);
    return HairDetection{{static_cast<int>(rgb[0]), static_cast<int>(rgb[1]), static_cast<int>(rgb[2])}, confidence};
}

} // namespace anichar
