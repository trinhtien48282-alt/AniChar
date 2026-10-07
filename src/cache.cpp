#include "anichar/cache.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace anichar {
namespace {
constexpr int currentVersion = 2;

void check(sqlite3* db, int result) {
    if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        check(db_, sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const { return stmt_; }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

void bindText(sqlite3* db, sqlite3_stmt* stmt, int index, const std::string& value) {
    check(db, sqlite3_bind_text(stmt, index, value.c_str(), static_cast<int>(value.size()), SQLITE_TRANSIENT));
}
void bindOptionalText(sqlite3* db, sqlite3_stmt* stmt, int index,
                      const std::optional<std::string>& value) {
    if (value) bindText(db, stmt, index, *value);
    else check(db, sqlite3_bind_null(stmt, index));
}
void bindOptionalDouble(sqlite3* db, sqlite3_stmt* stmt, int index,
                        const std::optional<double>& value) {
    if (value) check(db, sqlite3_bind_double(stmt, index, *value));
    else check(db, sqlite3_bind_null(stmt, index));
}
std::string textColumn(sqlite3_stmt* stmt, int column) {
    const auto* text = sqlite3_column_text(stmt, column);
    return text ? reinterpret_cast<const char*>(text) : std::string{};
}
std::optional<std::string> optionalTextColumn(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return std::nullopt;
    return textColumn(stmt, column);
}
std::optional<double> optionalDoubleColumn(sqlite3_stmt* stmt, int column) {
    if (sqlite3_column_type(stmt, column) == SQLITE_NULL) return std::nullopt;
    return sqlite3_column_double(stmt, column);
}
CacheRecord readRecord(sqlite3_stmt* stmt) {
    CacheRecord row;
    auto& c = row.character;
    c.id = sqlite3_column_int64(stmt, 0);
    c.name = textColumn(stmt, 1);
    row.url = textColumn(stmt, 2);
    row.imageUrl = textColumn(stmt, 3);
    row.imagePath = textColumn(stmt, 4);
    row.imageHash = textColumn(stmt, 5);
    if (sqlite3_column_type(stmt, 6) != SQLITE_NULL &&
        sqlite3_column_type(stmt, 7) != SQLITE_NULL &&
        sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
        c.rgb = Rgb{sqlite3_column_int(stmt, 6), sqlite3_column_int(stmt, 7), sqlite3_column_int(stmt, 8)};
    }
    if (sqlite3_column_type(stmt, 9) != SQLITE_NULL &&
        sqlite3_column_type(stmt, 10) != SQLITE_NULL &&
        sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
        c.lab = Lab{sqlite3_column_double(stmt, 9), sqlite3_column_double(stmt, 10), sqlite3_column_double(stmt, 11)};
    }
    c.confidence = optionalDoubleColumn(stmt, 12);
    if (sqlite3_column_type(stmt, 13) != SQLITE_NULL) c.version = sqlite3_column_int(stmt, 13);
    if (sqlite3_column_type(stmt, 14) != SQLITE_NULL) c.timestamp = sqlite3_column_double(stmt, 14);
    c.gender = optionalTextColumn(stmt, 15);
    row.infoJson = textColumn(stmt, 16);
    if (row.infoJson.empty()) row.infoJson = "{}";
    return row;
}
constexpr const char* allColumns =
    "mal_id,name,url,img_url,img_path,img_hash,r,g,b,lab_l,lab_a,lab_b,conf,version,ts,gender,info";
}

Cache::Cache(const std::string& path) {
    if (path != ":memory:") {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
    }
    const int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = db_ ? sqlite3_errmsg(db_) : "Unable to open SQLite cache";
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(message);
    }
    try {
        char* error = nullptr;
        const char* schema = "CREATE TABLE IF NOT EXISTS chars("
            "mal_id INTEGER PRIMARY KEY,name TEXT,url TEXT,img_url TEXT,img_path TEXT,img_hash TEXT,"
            "r INT,g INT,b INT,lab_l REAL,lab_a REAL,lab_b REAL,conf REAL,version INT,ts REAL,gender TEXT,info TEXT)";
        const int schemaRc = sqlite3_exec(db_, schema, nullptr, nullptr, &error);
        if (schemaRc != SQLITE_OK) {
            const std::string message = error ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
        for (const char* column : {"gender TEXT", "info TEXT"}) {
            const std::string sql = std::string("ALTER TABLE chars ADD COLUMN ") + column;
            if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
                const std::string message = error ? error : sqlite3_errmsg(db_);
                sqlite3_free(error);
                error = nullptr;
                if (message.find("duplicate column name") == std::string::npos)
                    throw std::runtime_error(message);
            }
        }
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

Cache::~Cache() { if (db_) sqlite3_close(db_); }

void Cache::save(const CacheRecord& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "INSERT OR REPLACE INTO chars VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    auto* s = stmt.get();
    const auto& c = record.character;
    check(db_, sqlite3_bind_int64(s, 1, c.id));
    bindText(db_, s, 2, c.name);
    bindText(db_, s, 3, record.url);
    bindText(db_, s, 4, record.imageUrl);
    bindText(db_, s, 5, record.imagePath);
    bindText(db_, s, 6, record.imageHash);
    const auto rgb = c.rgb;
    for (int i = 0; i < 3; ++i) {
        if (rgb) check(db_, sqlite3_bind_int(s, 7 + i, (*rgb)[i]));
        else check(db_, sqlite3_bind_null(s, 7 + i));
    }
    const auto lab = rgb ? std::optional<Lab>(toLab(*rgb)) : std::nullopt;
    const double values[] = {lab ? lab->l : 0.0, lab ? lab->a : 0.0, lab ? lab->b : 0.0};
    for (int i = 0; i < 3; ++i) {
        if (lab) check(db_, sqlite3_bind_double(s, 10 + i, values[i]));
        else check(db_, sqlite3_bind_null(s, 10 + i));
    }
    bindOptionalDouble(db_, s, 13, rgb ? c.confidence : std::nullopt);
    check(db_, sqlite3_bind_int(s, 14, currentVersion));
    const auto now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    check(db_, sqlite3_bind_double(s, 15, now));
    bindOptionalText(db_, s, 16, c.gender);
    bindText(db_, s, 17, record.infoJson.empty() ? "{}" : record.infoJson);
    check(db_, sqlite3_step(s));
}

std::optional<CacheRecord> Cache::get(std::int64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql = std::string("SELECT ") + allColumns + " FROM chars WHERE mal_id=?";
    Statement stmt(db_, sql.c_str());
    check(db_, sqlite3_bind_int64(stmt.get(), 1, id));
    const int rc = sqlite3_step(stmt.get());
    if (rc == SQLITE_DONE) return std::nullopt;
    check(db_, rc);
    return readRecord(stmt.get());
}

void Cache::backfill(std::int64_t id, const std::optional<std::string>& gender,
                     const std::string& infoJson) {
    std::lock_guard<std::mutex> lock(mutex_);
    Statement stmt(db_, "UPDATE chars SET gender=COALESCE(gender,?),info=COALESCE(info,?) WHERE mal_id=?");
    bindOptionalText(db_, stmt.get(), 1, gender);
    bindText(db_, stmt.get(), 2, infoJson);
    check(db_, sqlite3_bind_int64(stmt.get(), 3, id));
    check(db_, sqlite3_step(stmt.get()));
}

std::vector<CacheMatch> Cache::ranked(const RankOptions& options) const {
    std::vector<CacheRecord> rows;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string sql = std::string("SELECT ") + allColumns +
            " FROM chars WHERE r IS NOT NULL AND version=? AND conf>=? "
            "AND lower(name) LIKE lower(?) AND (?=0 OR gender='Female')";
        Statement stmt(db_, sql.c_str());
        check(db_, sqlite3_bind_int(stmt.get(), 1, options.currentVersion));
        check(db_, sqlite3_bind_double(stmt.get(), 2, options.minConfidence));
        bindText(db_, stmt.get(), 3, "%" + options.nameQuery + "%");
        check(db_, sqlite3_bind_int(stmt.get(), 4, options.girlsOnly ? 1 : 0));
        for (;;) {
            const int rc = sqlite3_step(stmt.get());
            if (rc == SQLITE_DONE) break;
            check(db_, rc);
            auto row = readRecord(stmt.get());
            QJsonParseError parseError;
            const auto json = QJsonDocument::fromJson(QByteArray::fromStdString(row.infoJson), &parseError);
            if (parseError.error != QJsonParseError::NoError)
                throw std::runtime_error("malformed cached info JSON");
            const auto hair = json.object().value(QStringLiteral("hair"));
            if (hair.isString() && !hair.toString().isEmpty()) row.character.listedHair = hair.toString().toStdString();
            rows.push_back(std::move(row));
        }
    }
    std::vector<CachedCharacter> characters;
    characters.reserve(rows.size());
    for (const auto& row : rows) characters.push_back(row.character);
    auto rankingOptions = options;
    rankingOptions.nameQuery.clear();
    rankingOptions.girlsOnly = false;
    auto matches = rank(characters, rankingOptions);
    std::vector<CacheMatch> result;
    result.reserve(matches.size());
    std::unordered_map<std::int64_t, const CacheRecord*> byId;
    byId.reserve(rows.size());
    for (const auto& row : rows) byId.emplace(row.character.id, &row);
    for (auto& match : matches) {
        const auto it = byId.find(match.character.id);
        if (it != byId.end()) result.push_back({*it->second, match.distance, match.similarity});
    }
    return result;
}

} // namespace anichar
