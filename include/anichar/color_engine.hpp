#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace anichar {

using Rgb = std::array<int, 3>;
struct Hsv { int hue, saturation, value; friend bool operator==(const Hsv& x, const Hsv& y) { return x.hue == y.hue && x.saturation == y.saturation && x.value == y.value; } };
struct Hsl { int hue, saturation, lightness; friend bool operator==(const Hsl& x, const Hsl& y) { return x.hue == y.hue && x.saturation == y.saturation && x.lightness == y.lightness; } };
struct Lab { double l, a, b; };

enum class SortOrder { BestMatch, HighestConfidence, Name, NewestAnalysis };
enum class DistanceMethod { CIEDE2000, CIE76 };

struct CachedCharacter {
    std::int64_t id = 0;
    std::string name;
    std::optional<Rgb> rgb;
    std::optional<Lab> lab;
    std::optional<double> confidence;
    int version = 2;
    double timestamp = 0.0;
    std::optional<std::string> gender;
    std::optional<std::string> listedHair;
};

struct RankOptions {
    Rgb target{107, 182, 217};
    double maxDistance = 25.0;
    double minConfidence = 0.0;
    std::string nameQuery;
    bool girlsOnly = true;
    bool rejectHairConflicts = true;
    SortOrder sort = SortOrder::BestMatch;
    DistanceMethod distance = DistanceMethod::CIEDE2000;
    int currentVersion = 2;
};

struct Match {
    CachedCharacter character;
    double distance = 0.0;
    double similarity = 0.0;
};

Rgb parseColor(const std::string& text);
std::string toHex(const Rgb& rgb);
Hsv toHsv(const Rgb& rgb);
Hsl toHsl(const Rgb& rgb);
Lab toLab(const Rgb& rgb);
double deltaE(const Lab& first, const Lab& second, DistanceMethod method = DistanceMethod::CIEDE2000);
double similarity(double distance, double scale = 50.0);
std::string baseColor(const Rgb& rgb);
std::string colorName(const Rgb& rgb);
bool hairConflict(const Rgb& rgb, const std::string& listedHair);
bool nameMatches(const std::string& name, const std::string& query);
std::vector<Match> rank(const std::vector<CachedCharacter>& rows, const RankOptions& options = {});

} // namespace anichar
