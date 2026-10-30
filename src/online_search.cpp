#include "anichar/online_search.hpp"

#include "anichar/cache.hpp"
#include "anichar/hair_detector.hpp"

#include <QCryptographicHash>
#include <QBuffer>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimer>
#include <QThreadPool>
#include <QUrlQuery>

#include <algorithm>
#include <optional>
#include <vector>

namespace anichar {
namespace {
constexpr int pageSize = 25;
constexpr int maxImageBytes = 5'000'000;
constexpr int analysisVersion = 2;
const QByteArray aniListQuery = QByteArrayLiteral(
    "query($p:Int,$n:Int,$q:String){Page(page:$p,perPage:$n){characters(search:$q,sort:FAVOURITES_DESC){"
    "id name{full} gender age dateOfBirth{year month day} bloodType favourites description image{large} siteUrl}}}");

struct Character {
    qint64 id = 0;
    QString name, url, imageUrl, gender;
    QJsonObject info;
};

QString cleanText(QString s, int limit = 400) {
    s.remove(QRegularExpression(QStringLiteral("~!.*?!~"), QRegularExpression::DotMatchesEverythingOption));
    s.replace(QRegularExpression(QStringLiteral("<br\\s*/?>"), QRegularExpression::CaseInsensitiveOption), "\n");
    s.remove(QRegularExpression(QStringLiteral("<[^>]+>")));
    s.replace(QRegularExpression(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)")), QStringLiteral("\\1"));
    s.remove(QRegularExpression(QStringLiteral("[_*]{1,3}")));
    s.replace(QRegularExpression(QStringLiteral("[ \\t]+")), " ");
    s = s.trimmed();
    if (limit > 0 && s.size() > limit) s = s.left(limit).trimmed() + "...";
    return s;
}

QStringList hairFamilies(const QString& text) {
    static const QHash<QString, QString> words = {
        {"black","black"},{"white","white"},{"gray","gray"},{"grey","gray"},{"silver","gray"},
        {"red","red"},{"crimson","red"},{"scarlet","red"},{"orange","orange"},
        {"yellow","yellow"},{"blonde","yellow"},{"blond","yellow"},{"golden","yellow"},
        {"brown","brown"},{"chestnut","brown"},{"auburn","brown"},{"green","green"},
        {"teal","cyan"},{"aqua","cyan"},{"turquoise","cyan"},{"cyan","cyan"},{"blue","blue"},
        {"navy","blue"},{"purple","purple"},{"violet","purple"},{"lavender","purple"},
        {"pink","pink"},{"magenta","pink"}};
    QStringList out;
    const auto wordsFound = QRegularExpression(QStringLiteral("[a-z]+"), QRegularExpression::CaseInsensitiveOption)
        .globalMatch(text.toLower());
    auto it = wordsFound;
    while (it.hasNext()) {
        const auto key = it.next().captured();
        const auto value = words.value(key);
        if (!value.isEmpty() && !out.contains(value)) out.push_back(value);
    }
    return out;
}

QJsonObject facts(const QString& about) {
    QJsonObject info;
    for (const auto& field : {qMakePair(QStringLiteral("age"), QStringLiteral("age")),
                              qMakePair(QStringLiteral("birthday"), QStringLiteral("(?:birthday|birthdate|birth date|date of birth"))}) {
        const auto re = QRegularExpression(QStringLiteral("^\\s*%1\\s*:\\s*([^\\n]{1,30})")
            .arg(field.second), QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
        const auto m = re.match(about);
        if (m.hasMatch()) info.insert(field.first, m.captured(1).trimmed());
    }
    auto m = QRegularExpression(QStringLiteral("^\\s*hair(?:\\s*colou?r)?\\s*:\\s*([^\\n]{1,40})"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption).match(about);
    if (m.hasMatch() && !hairFamilies(m.captured(1)).isEmpty()) {
        info.insert("hair", m.captured(1).trimmed().toLower());
    } else {
        const auto matches = QRegularExpression(QStringLiteral("\\b([a-z]+)[- ]haired\\b|\\b([a-z]+) hair\\b"),
            QRegularExpression::CaseInsensitiveOption).globalMatch(about);
        auto it = matches;
        while (it.hasNext()) {
            const auto hit = it.next();
            const auto word = hit.captured(1).isEmpty() ? hit.captured(2) : hit.captured(1);
            if (!hairFamilies(word).isEmpty()) { info.insert("hair", word.toLower()); break; }
        }
    }
    info.insert("about", about);
    return info;
}

QJsonValue numericOrNull(const QJsonValue& v) {
    return v.isDouble() ? QJsonValue(static_cast<qint64>(v.toDouble())) : QJsonValue(QJsonValue::Null);
}

std::optional<Character> parseJikanCharacter(const QJsonObject& c) {
    const auto image = c.value("images").toObject().value("jpg").toObject().value("image_url").toString();
    const auto id = c.value("mal_id").toVariant().toLongLong();
    const auto name = c.value("name").toString();
    if (id <= 0 || name.isEmpty() || image.isEmpty()) return std::nullopt;
    Character out;
    out.id = id; out.name = name; out.url = c.value("url").toString(); out.imageUrl = image;
    out.info = facts(cleanText(c.value("about").toString()));
    out.info.insert("favs", numericOrNull(c.value("favorites")));
    QJsonArray nicknames;
    const auto source = c.value("nicknames").toArray();
    for (qsizetype i = 0; i < std::min<qsizetype>(2, source.size()); ++i) nicknames.append(source[i]);
    out.info.insert("nicknames", nicknames);
    return out;
}

std::optional<Character> parseAniListCharacter(const QJsonObject& c) {
    const auto id = c.value("id").toVariant().toLongLong();
    const auto name = c.value("name").toObject().value("full").toString();
    const auto image = c.value("image").toObject().value("large").toString();
    if (id <= 0 || name.isEmpty() || image.isEmpty() || image.contains("default.jpg", Qt::CaseInsensitive)) return std::nullopt;
    Character out;
    out.id = -id; out.name = name; out.gender = c.value("gender").toString();
    out.url = c.value("siteUrl").toString(); out.imageUrl = image;
    out.info = facts(cleanText(c.value("description").toString()));
    out.info.insert("favs", numericOrNull(c.value("favourites")));
    if (c.value("age").isString() || c.value("age").isDouble()) out.info.insert("age", c.value("age").toVariant().toString());
    const auto dob = c.value("dateOfBirth").toObject();
    const auto month = dob.value("month").toInt(), day = dob.value("day").toInt(), year = dob.value("year").toInt();
    static const QStringList months = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    if (month >= 1 && month <= 12) {
        QString date = months[month - 1];
        if (day > 0) date += QStringLiteral(" %1").arg(day);
        if (year > 0) date += QStringLiteral(", %1").arg(year);
        out.info.insert("birthday", date);
    }
    if (!c.value("bloodType").toString().isEmpty()) out.info.insert("blood", c.value("bloodType").toString());
    return out;
}

}

class OnlineSearch::Impl {
public:
    struct Source { QString name; QUrl base; bool aniList = false; qint64 lastRequest = -1; int page = 1; QVector<Character> candidates; int index = 0; bool done = false; };
    OnlineSearch* owner;
    Cache& cache;
    QNetworkAccessManager network;
    QThreadPool imageWorkers;
    QUrl jikanBase, aniListEndpoint;
    OnlineSearchOptions options;
    Status status;
    Progress progress;
    Finished finished;
    QVector<Source> sources;
    QSet<QString> seen;
    QList<QNetworkReply*> replies;
    QElapsedTimer clock;
    OnlineSearchProgress counts;
    QStringList errors;
    bool stopped = true, completionSent = true;
    quint64 generation = 0;

    Impl(OnlineSearch* owner, Cache& cache, QUrl jikan, QUrl aniList)
        : owner(owner), cache(cache), network(owner), jikanBase(std::move(jikan)), aniListEndpoint(std::move(aniList)) {
        imageWorkers.setMaxThreadCount(4);
    }

    ~Impl() { imageWorkers.waitForDone(); }

    void start(const OnlineSearchOptions& opts, Status onStatus, Progress onProgress, Finished onFinished) {
        cancel();
        ++generation;
        options = opts; status = std::move(onStatus); progress = std::move(onProgress); finished = std::move(onFinished);
        counts = {}; errors.clear(); seen.clear(); sources.clear(); stopped = false; completionSent = false;
        clock.start();
        if (options.target < 1 || (!options.jikan && !options.aniList) || (options.girlsOnly && !options.aniList)) {
            errors << (options.girlsOnly && !options.aniList ? QStringLiteral("Girls-only search requires AniList, which provides gender.")
                                                              : QStringLiteral("Select a search source and a positive result target."));
            finish(); return;
        }
        if (options.jikan && !options.girlsOnly) sources.push_back({"Jikan", jikanBase});
        if (options.aniList) sources.push_back({"AniList", aniListEndpoint, true});
        if (options.jikan && options.girlsOnly && status)
            status(QStringLiteral("Jikan skipped: it does not provide gender. Searching AniList."));
        for (int i = 0; i < sources.size(); ++i) fetchPage(i);
    }

    void cancel() {
        if (stopped) return;
        stopped = true;
        errors << QStringLiteral("Search stopped by user.");
        const auto activeReplies = replies;
        replies.clear();
        for (auto* reply : activeReplies) if (reply && reply->isRunning()) reply->abort();
        finish();
    }

    void finish() {
        if (completionSent) return;
        completionSent = true; stopped = true;
        ++generation;
        auto callback = finished;
        const auto activeReplies = replies;
        replies.clear();
        for (auto* reply : activeReplies) if (reply && reply->isRunning()) reply->abort();
        if (callback) callback(errors);
    }

    void request(int sourceIndex, QNetworkRequest req, QByteArray body, int retry, int timeoutMs,
                 std::function<void(QByteArray)> success, std::function<void(QString)> failure,
                 bool image = false) {
        if (stopped) return;
        const auto requestGeneration = generation;
        auto& source = sources[sourceIndex];
        const qint64 delayMs = image || source.lastRequest < 0 ? 0 :
            ((source.aniList ? 1000 : 1100) - (clock.elapsed() - source.lastRequest));
        if (!image && delayMs > 0) {
            QTimer::singleShot(static_cast<int>(delayMs), owner,
                [this, requestGeneration, sourceIndex, req = std::move(req), body = std::move(body), retry, timeoutMs,
                 success = std::move(success), failure = std::move(failure), image]() mutable {
                    if (generation != requestGeneration) return;
                    request(sourceIndex, std::move(req), std::move(body), retry, timeoutMs,
                            std::move(success), std::move(failure), image);
                });
            return;
        }
        if (!image) source.lastRequest = clock.elapsed();
        req.setRawHeader("User-Agent", "AnimeHairColorFinder/1.0 (personal desktop app)");
        req.setTransferTimeout(timeoutMs);
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = body.isNull() ? network.get(req) : network.post(req, body);
        replies.push_back(reply);
        auto bytes = std::make_shared<QByteArray>();
        auto tooLarge = std::make_shared<bool>(false);
        if (image) connect(reply, &QIODevice::readyRead, owner, [reply, bytes, tooLarge] {
            const auto contentLength = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
            if (contentLength > maxImageBytes) { *tooLarge = true; reply->abort(); return; }
            const auto remaining = maxImageBytes + 1 - bytes->size();
            if (remaining > 0) bytes->append(reply->read(remaining));
            if (bytes->size() > maxImageBytes) { *tooLarge = true; reply->abort(); }
        });
        connect(reply, &QNetworkReply::finished, owner,
            [this, reply, bytes, tooLarge, requestGeneration, sourceIndex, req, body, retry, timeoutMs,
             success = std::move(success), failure = std::move(failure), image]() mutable {
                replies.removeOne(reply);
                const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const auto headers = reply->rawHeader("Retry-After");
                if (!*tooLarge) {
                    const auto remaining = maxImageBytes + 1 - bytes->size();
                    if (remaining > 0) bytes->append(reply->read(remaining));
                    if (bytes->size() > maxImageBytes) *tooLarge = true;
                }
                const auto networkError = reply->error();
                const auto message = reply->errorString();
                const auto contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
                reply->deleteLater();
                if (stopped || generation != requestGeneration) return;
                if (*tooLarge || (image && bytes->size() > maxImageBytes)) { failure(QStringLiteral("image too large (maximum 5 MB)")); return; }
                const bool serverBusy = statusCode == 429 || statusCode >= 500;
                const bool connectionFailure = networkError != QNetworkReply::NoError && statusCode == 0;
                const int retryLimit = serverBusy ? 4 : 2;
                if ((serverBusy || connectionFailure) && retry < retryLimit) {
                    bool ok = false;
                    double delay = headers.toDouble(&ok);
                    if (!ok) delay = std::pow(2.0, retry);
                    delay = std::clamp(delay, 0.0, 30.0);
                    if (status) status(QStringLiteral("%1 busy or disconnected; retry %2/%3 in %4 s.")
                        .arg(sources[sourceIndex].name).arg(retry + 1).arg(retryLimit + 1).arg(delay, 0, 'f', 0));
                    QTimer::singleShot(static_cast<int>(delay * 1000), owner,
                        [this, requestGeneration, sourceIndex, req, body, retry, timeoutMs, success = std::move(success),
                         failure = std::move(failure), image]() mutable {
                            if (generation != requestGeneration) return;
                            request(sourceIndex, req, body, retry + 1, timeoutMs,
                                    std::move(success), std::move(failure), image);
                        });
                    return;
                }
                if (networkError != QNetworkReply::NoError || statusCode < 200 || statusCode >= 300) {
                    failure(QStringLiteral("HTTP %1: %2").arg(statusCode).arg(message)); return;
                }
                if (image && !contentType.startsWith("image/", Qt::CaseInsensitive)) {
                    failure(QStringLiteral("not an image (content-type %1)").arg(contentType)); return;
                }
                success(*bytes);
            });
    }

    void fetchPage(int index) {
        if (stopped || counts.newlyAnalyzed >= options.target || sources[index].done) return;
        auto& source = sources[index];
        QNetworkRequest req;
        QByteArray body;
        if (source.aniList) {
            req = QNetworkRequest(aniListEndpoint);
            req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
            QJsonObject variables{{"p", source.page}, {"n", pageSize},
                                  {"q", options.query.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(options.query)}};
            body = QJsonDocument(QJsonObject{{"query", QString::fromLatin1(aniListQuery)}, {"variables", variables}})
                .toJson(QJsonDocument::Compact);
        } else {
            QUrl url = source.base.resolved(QUrl(options.query.isEmpty() ? "top/characters" : "characters"));
            QUrlQuery query;
            if (!options.query.isEmpty()) {
                query.addQueryItem("q", options.query);
            }
            query.addQueryItem("page", QString::number(source.page)); query.addQueryItem("limit", QString::number(pageSize));
            if (!options.query.isEmpty()) { query.addQueryItem("order_by", "favorites"); query.addQueryItem("sort", "desc"); }
            url.setQuery(query); req = QNetworkRequest(url);
        }
        request(index, req, body, 0, source.aniList ? 8000 : 5000,
            [this, index](QByteArray bytes) {
                QJsonParseError parseError;
                const auto document = QJsonDocument::fromJson(bytes, &parseError);
                if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
                    sourceFailed(index, QStringLiteral("malformed JSON")); return;
                }
                const auto root = document.object();
                if (sources[index].aniList) {
                    if (!root.value("errors").toArray().isEmpty()) {
                        sourceFailed(index, root.value("errors").toArray().first().toObject().value("message").toString()); return;
                    }
                    const auto chars = root.value("data").toObject().value("Page").toObject().value("characters").toArray();
                    for (const auto& item : chars) if (item.isObject()) {
                        const auto c = parseAniListCharacter(item.toObject());
                        if (c && (!options.girlsOnly || c->gender == "Female")) sources[index].candidates.push_back(*c);
                    }
                } else {
                    const auto chars = root.value("data").toArray();
                    for (const auto& item : chars) if (item.isObject()) {
                        const auto c = parseJikanCharacter(item.toObject());
                        if (c) sources[index].candidates.push_back(*c);
                    }
                }
                const bool emptyPage = sources[index].aniList
                    ? root.value("data").toObject().value("Page").toObject().value("characters").toArray().isEmpty()
                    : root.value("data").toArray().isEmpty();
                if (emptyPage) { sources[index].done = true; checkFinished(); return; }
                if (sources[index].candidates.isEmpty()) {
                    if (sources[index].page >= std::max(8, options.target / 5)) { sources[index].done = true; checkFinished(); }
                    else { ++sources[index].page; fetchPage(index); }
                    return;
                }
                sources[index].index = 0; processNext(index);
            }, [this, index](QString error) { sourceFailed(index, error); });
    }

    void processNext(int index) {
        if (stopped) return;
        if (counts.newlyAnalyzed >= options.target) { finish(); return; }
        auto& source = sources[index];
        if (source.index >= source.candidates.size()) {
            source.candidates.clear();
            if (source.page >= std::max(8, options.target / 5)) { source.done = true; checkFinished(); return; }
            ++source.page;
            fetchPage(index); return;
        }
        const auto character = source.candidates[source.index++];
        const auto key = character.name.toCaseFolded();
        if (seen.contains(key)) { processNext(index); return; }
        seen.insert(key);
        try {
            const auto cached = cache.get(character.id);
            const auto imagePath = QDir::homePath() + QStringLiteral("/.anime_hair_finder/images/%1.jpg").arg(character.id);
            if (cached && cached->imageUrl == character.imageUrl && cached->character.version == analysisVersion) {
                if ((!character.gender.isEmpty() && !cached->character.gender) ||
                    (cached->infoJson == "{}" && !character.info.isEmpty()))
                    cache.backfill(character.id, character.gender.isEmpty() ? std::nullopt : std::optional<std::string>(character.gender.toStdString()),
                                   QString::fromUtf8(QJsonDocument(character.info).toJson(QJsonDocument::Compact)).toStdString());
                ++counts.checked; reportProgress(); processNext(index); return;
            }
            if (cached && cached->imageUrl == character.imageUrl && QFileInfo::exists(QString::fromStdString(cached->imagePath))) {
                try {
                    const auto detection = detectHair(loadImage(QString::fromStdString(cached->imagePath)));
                    save(character, cached->imagePath, QByteArray::fromHex(QByteArray::fromStdString(cached->imageHash)), detection);
                    ++counts.checked; ++counts.newlyAnalyzed; reportProgress(); processNext(index); return;
                } catch (...) { /* Re-download an unreadable saved thumbnail. */ }
            }
            QNetworkRequest req(QUrl(character.imageUrl));
            request(index, req, {}, 0, 8000, [this, index, character, imagePath](QByteArray bytes) {
                analyzeDownloaded(index, character, imagePath, std::move(bytes));
            }, [this, index, name = character.name](QString error) {
                ++counts.checked; ++counts.failed;
                if (status) status(QStringLiteral("Skipped %1: %2").arg(name, error));
                reportProgress(); processNext(index);
            }, true);
        } catch (const std::exception& e) {
            ++counts.checked; ++counts.failed;
            if (status) status(QStringLiteral("Skipped %1: %2").arg(character.name, QString::fromUtf8(e.what())));
            reportProgress(); processNext(index);
        }
    }

    void analyzeDownloaded(int index, Character character, QString imagePath, QByteArray bytes) {
        const auto epoch = generation;
        imageWorkers.start(QRunnable::create([this, epoch, index, character = std::move(character),
                                              imagePath = std::move(imagePath), bytes = std::move(bytes)]() mutable {
            std::optional<HairDetection> detection;
            QByteArray thumbnail;
            QString analysisError, thumbnailError;
            try {
                QImage preview;
                detection = detectHair(decodeImage(bytes, &preview));
                QBuffer buffer(&thumbnail);
                if (!buffer.open(QIODevice::WriteOnly) || !preview.save(&buffer, "JPEG", 85))
                    thumbnailError = QStringLiteral("could not encode card thumbnail");
            } catch (const std::exception& e) {
                analysisError = QString::fromUtf8(e.what());
            }
            QMetaObject::invokeMethod(owner,
                [this, epoch, index, character = std::move(character), imagePath = std::move(imagePath),
                 bytes = std::move(bytes), detection = std::move(detection), thumbnail = std::move(thumbnail),
                 analysisError = std::move(analysisError), thumbnailError = std::move(thumbnailError)]() mutable {
                    if (stopped || generation != epoch) return;
                    if (!analysisError.isEmpty() && status)
                        status(QStringLiteral("Could not analyze %1: %2").arg(character.name, analysisError));
                    try {
                        if (!thumbnail.isEmpty()) {
                            QDir().mkpath(QFileInfo(imagePath).absolutePath());
                            QSaveFile file(imagePath);
                            if (!file.open(QIODevice::WriteOnly) || file.write(thumbnail) != thumbnail.size() || !file.commit())
                                throw std::runtime_error("Could not save cached thumbnail");
                        } else if (thumbnailError.isEmpty()) {
                            thumbnailError = QStringLiteral("image decoder returned an empty thumbnail");
                        }
                        if (!thumbnailError.isEmpty() && status)
                            status(QStringLiteral("Thumbnail unavailable for %1: %2").arg(character.name, thumbnailError));
                        save(character, imagePath.toStdString(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), detection);
                        ++counts.checked; ++counts.newlyAnalyzed;
                    } catch (const std::exception& e) {
                        ++counts.checked; ++counts.failed;
                        if (status) status(QStringLiteral("Could not cache %1: %2").arg(character.name, QString::fromUtf8(e.what())));
                    }
                    reportProgress(); processNext(index);
                }, Qt::QueuedConnection);
        }));
    }

    void reanalyzeStale(ReanalysisFinished callback) {
        std::vector<StaleCacheRecord> rows;
        try { rows = cache.staleAnalyses(); }
        catch (const std::exception& e) {
            if (callback) callback(0, QString::fromUtf8(e.what()));
            return;
        }
        if (rows.empty()) { if (callback) callback(0, {}); return; }
        struct Batch { int completed = 0; QString error; ReanalysisFinished callback; int total = 0; };
        const auto batch = std::make_shared<Batch>();
        batch->callback = std::move(callback);
        batch->total = static_cast<int>(rows.size());
        for (const auto& row : rows) {
            imageWorkers.start(QRunnable::create([this, row, batch] {
                std::optional<HairDetection> detection;
                try { detection = detectHair(loadImage(QString::fromStdString(row.imagePath))); }
                catch (...) { }
                QMetaObject::invokeMethod(owner, [this, row, batch, detection] {
                    try { cache.updateAnalysis(row.id, detection ? std::optional<Rgb>(detection->rgb) : std::nullopt,
                                               detection ? std::optional<double>(detection->confidence) : std::nullopt); }
                    catch (const std::exception& e) { if (batch->error.isEmpty()) batch->error = QString::fromUtf8(e.what()); }
                    if (++batch->completed == batch->total && batch->callback)
                        batch->callback(batch->total, batch->error);
                }, Qt::QueuedConnection);
            }));
        }
    }

    void save(const Character& c, const std::string& imagePath, const QByteArray& hash,
              const std::optional<HairDetection>& detection) {
        CacheRecord record;
        record.character.id = c.id; record.character.name = c.name.toStdString();
        if (!c.gender.isEmpty()) record.character.gender = c.gender.toStdString();
        if (detection) {
            record.character.rgb = detection->rgb; record.character.confidence = detection->confidence;
            record.character.lab = toLab(detection->rgb);
        }
        record.url = c.url.toStdString(); record.imageUrl = c.imageUrl.toStdString();
        record.imagePath = imagePath; record.imageHash = hash.toHex().toStdString();
        record.infoJson = QJsonDocument(c.info).toJson(QJsonDocument::Compact).toStdString();
        cache.save(record);
    }

    void reportProgress() { if (progress) progress(counts); }
    void sourceFailed(int index, const QString& error) {
        if (sources[index].done) return;
        sources[index].done = true;
        errors << QStringLiteral("%1: %2").arg(sources[index].name, error.left(150));
        if (status) status(QStringLiteral("%1 unavailable: %2").arg(sources[index].name, error.left(150)));
        checkFinished();
    }
    void checkFinished() {
        if (counts.newlyAnalyzed >= options.target) { finish(); return; }
        if (std::all_of(sources.begin(), sources.end(), [](const Source& s) { return s.done; })) finish();
    }
};

OnlineSearch::OnlineSearch(Cache& cache, QObject* parent)
    : OnlineSearch(cache, jikanBaseUrl(), aniListUrl(), parent) {}
OnlineSearch::OnlineSearch(Cache& cache, QUrl jikanBase, QUrl aniList, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, cache, std::move(jikanBase), std::move(aniList))) {}
OnlineSearch::~OnlineSearch() = default;
void OnlineSearch::start(const OnlineSearchOptions& options, Status status, Progress progress, Finished finished) {
    impl_->start(options, std::move(status), std::move(progress), std::move(finished));
}
void OnlineSearch::cancel() { impl_->cancel(); }
void OnlineSearch::reanalyzeStale(ReanalysisFinished finished) { impl_->reanalyzeStale(std::move(finished)); }
QUrl OnlineSearch::jikanBaseUrl() { return QUrl(QStringLiteral("https://api.jikan.moe/v4/")); }
QUrl OnlineSearch::aniListUrl() { return QUrl(QStringLiteral("https://graphql.anilist.co")); }
}
