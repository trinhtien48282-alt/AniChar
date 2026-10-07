#include "anichar/cache.hpp"

#include <sqlite3.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace anichar;
namespace {
int checks = 0;
void check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) throw std::runtime_error(label);
}
CacheRecord record(std::int64_t id, const std::string& name, Rgb rgb,
                   double confidence, std::string gender, std::string hair = {}) {
    CacheRecord row;
    row.character.id = id;
    row.character.name = name;
    row.character.rgb = rgb;
    row.character.confidence = confidence;
    row.character.gender = gender;
    row.url = "https://example.test/" + std::to_string(id);
    row.imageUrl = "https://example.test/image/" + std::to_string(id);
    row.imagePath = "images/" + std::to_string(id) + ".jpg";
    row.imageHash = "hash-" + std::to_string(id);
    row.infoJson = hair.empty() ? "{\"about\":\"fixture\"}" : "{\"hair\":\"" + hair + "\"}";
    return row;
}
}

int main() {
    try {
        Cache cache(":memory:");
        check(!cache.get(1), "new cache initializes and starts empty");

        auto exact = record(3, "Mikasa Ackerman", {255, 0, 0}, .8, "Female", "red hair");
        auto conflict = record(2, "Sasha", {255, 0, 0}, .95, "Female", "blue hair");
        auto lowConfidence = record(1, "Annie", {255, 0, 0}, .49, "Female");
        auto male = record(4, "Jean", {255, 0, 0}, .99, "Male");
        auto lowerCaseFemale = record(7, "Historia", {255, 0, 0}, .99, "female");
        auto distant = record(8, "Distant", {0, 0, 255}, .99, "Female");
        auto noAnalysis = record(6, "Armin", {255, 0, 0}, .9, "Female");
        noAnalysis.character.rgb.reset();
        noAnalysis.character.confidence.reset();
        cache.save(exact);
        cache.save(conflict);
        cache.save(lowConfidence);
        cache.save(male);
        cache.save(lowerCaseFemale);
        cache.save(distant);
        cache.save(noAnalysis);

        const auto loaded = cache.get(3);
        check(loaded && loaded->character.name == exact.character.name, "insert/read name round trip");
        check(loaded && loaded->character.rgb == exact.character.rgb, "insert/read RGB round trip");
        check(loaded && loaded->character.lab && std::abs(loaded->character.lab->l - toLab({255, 0, 0}).l) < 1e-12,
              "cache stores Python-compatible Lab from RGB");
        check(loaded && loaded->character.confidence == exact.character.confidence, "insert/read confidence round trip");
        check(loaded && loaded->character.gender == exact.character.gender && loaded->infoJson == exact.infoJson,
              "insert/read metadata round trip");
        check(loaded && loaded->url == exact.url && loaded->imageUrl == exact.imageUrl &&
              loaded->imagePath == exact.imagePath && loaded->imageHash == exact.imageHash,
              "insert/read source and image fields round trip");
        check(loaded && loaded->character.version == 2 && loaded->character.timestamp > 0,
              "save assigns current version and timestamp");

        RankOptions options;
        options.target = {255, 0, 0};
        options.maxDistance = 0;
        options.minConfidence = .5;
        options.nameQuery = "MIKASA";
        options.girlsOnly = true;
        auto found = cache.ranked(options);
        check(found.size() == 1 && found[0].record.character.id == 3,
              "version, confidence, exact Female, case-insensitive LIKE and zero-distance filtering");
        check(found[0].distance == 0 && found[0].similarity == 100, "cached exact-match score");

        options.nameQuery.clear();
        options.minConfidence = 0;
        found = cache.ranked(options);
        check(found.size() == 2 && found[0].record.character.id == 1 && found[1].record.character.id == 3,
              "listed hair conflict is checked against target and low confidence remains filtered");

        options.girlsOnly = false;
        options.nameQuery = "M_kasa";
        options.minConfidence = .5;
        found = cache.ranked(options);
        check(found.size() == 1 && found[0].record.character.id == 3,
              "SQLite underscore wildcard name matching");
        options.nameQuery = "%";
        options.maxDistance = -1;
        check(cache.ranked(options).empty(), "distance cutoff excludes all positive and zero distances");

        options.maxDistance = 0;
        options.nameQuery.clear();
        options.girlsOnly = false;
        options.rejectHairConflicts = false;
        const auto tied = cache.ranked(options);
        check(tied.size() == 4 && tied[0].record.character.id == 2 && tied[1].record.character.id == 3 &&
              tied[2].record.character.id == 4 && tied[3].record.character.id == 7,
              "stable ties preserve SQLite's observed primary-key scan order");

        cache.backfill(3, std::string("Female"), "{\"hair\":\"black\"}");
        const auto preserved = cache.get(3);
        check(preserved && preserved->infoJson == exact.infoJson, "backfill preserves non-null cached info via COALESCE");

        const auto emptyPath = std::filesystem::temp_directory_path() / "anichar-legacy-cache-test.db";
        std::filesystem::remove(emptyPath);

        const auto versionPath = std::filesystem::temp_directory_path() / "anichar-version-cache-test.db";
        std::filesystem::remove(versionPath);
        {
            Cache versioned(versionPath.string());
            versioned.save(record(20, "Old analysis", {255, 0, 0}, .9, "Female"));
            sqlite3* rawVersionDb = nullptr;
            check(sqlite3_open(versionPath.string().c_str(), &rawVersionDb) == SQLITE_OK, "open version fixture");
            const int updateResult = sqlite3_exec(rawVersionDb, "UPDATE chars SET version=1 WHERE mal_id=20", nullptr, nullptr, nullptr);
            sqlite3_close(rawVersionDb);
            check(updateResult == SQLITE_OK && !versioned.ranked(options).size(), "ranked excludes stale analysis versions");
        }
        std::filesystem::remove(versionPath);
        sqlite3* raw = nullptr;
        check(sqlite3_open(emptyPath.string().c_str(), &raw) == SQLITE_OK, "create legacy SQLite fixture");
        char* error = nullptr;
        const char* legacy = "CREATE TABLE chars(mal_id INTEGER PRIMARY KEY,name TEXT,url TEXT,img_url TEXT,img_path TEXT,img_hash TEXT,r INT,g INT,b INT,lab_l REAL,lab_a REAL,lab_b REAL,conf REAL,version INT,ts REAL)";
        const int legacyResult = sqlite3_exec(raw, legacy, nullptr, nullptr, &error);
        if (legacyResult != SQLITE_OK) {
            const std::string message = error ? error : "legacy schema creation failed";
            sqlite3_free(error);
            sqlite3_close(raw);
            throw std::runtime_error(message);
        }
        sqlite3_close(raw);
        {
            Cache migrated(emptyPath.string());
            const auto absent = migrated.get(9);
            check(!absent, "legacy schema migrates by adding gender and info columns");
        }
        std::filesystem::remove(emptyPath);

        Cache malformed(":memory:");
        auto bad = record(10, "Bad JSON", {255, 0, 0}, .9, "Female");
        bad.infoJson = "{";
        malformed.save(bad);
        bool rejected = false;
        try { (void)malformed.ranked(options); }
        catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "malformed info JSON fails like Python json.loads");

        std::cout << "Passed " << checks << " SQLite cache checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
