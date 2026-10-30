#pragma once

#include "anichar/color_engine.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace anichar {

struct CacheRecord {
    CachedCharacter character;
    std::string url;
    std::string imageUrl;
    std::string imagePath;
    std::string imageHash;
    std::string infoJson = "{}";
};

struct CacheMatch {
    CacheRecord record;
    double distance = 0.0;
    double similarity = 0.0;
};

struct StaleCacheRecord {
    std::int64_t id = 0;
    std::string imagePath;
};

class Cache {
public:
    explicit Cache(const std::string& path);
    ~Cache();
    Cache(const Cache&) = delete;
    Cache& operator=(const Cache&) = delete;

    void save(const CacheRecord& record);
    std::optional<CacheRecord> get(std::int64_t id) const;
    void backfill(std::int64_t id, const std::optional<std::string>& gender,
                  const std::string& infoJson);
    std::vector<StaleCacheRecord> staleAnalyses() const;
    void updateAnalysis(std::int64_t id, const std::optional<Rgb>& rgb,
                        const std::optional<double>& confidence);
    std::vector<CacheMatch> ranked(const RankOptions& options = {}) const;

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

} // namespace anichar
