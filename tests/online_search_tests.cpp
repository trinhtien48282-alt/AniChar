#include "anichar/cache.hpp"
#include "anichar/online_search.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QTemporaryDir>
#include <sqlite3.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace anichar;

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class MockServer {
public:
    QTcpServer server;
    QByteArray png;
    QStringList requests;
    int jikanCalls = 0;
    int jikanPagesServed = 0;
    int jikan429Responses = 0;
    int image429Responses = 0;
    bool malformedJikan = false;
    bool missingJikanImage = false;
    bool wrongImageType = false;
    bool oversizedImage = false;
    int responseDelayMs = 0;
    int disconnectJikanResponses = 0;
    int aniListCalls = 0;

    MockServer() {
        QImage image(64, 64, QImage::Format_RGB32);
        image.fill(qRgb(245, 245, 245));
        for (int y = 0; y < 31; ++y) for (int x = 8; x < 56; ++x) image.setPixel(x, y, qRgb(25, 80, 210));
        QBuffer buffer(&png);
        check(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "create HTTP image fixture");
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (auto* socket = server.nextPendingConnection()) {
                auto* requestBytes = new QByteArray;
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, requestBytes] {
                    requestBytes->append(socket->readAll());
                    const auto headerEnd = requestBytes->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    const auto header = requestBytes->left(headerEnd);
                    int bodyLength = 0;
                    const auto marker = header.toLower().indexOf("content-length:");
                    if (marker >= 0) {
                        const auto end = header.indexOf("\r\n", marker);
                        bodyLength = header.mid(marker + 15, end < 0 ? -1 : end - marker - 15).trimmed().toInt();
                    }
                    if (requestBytes->size() < headerEnd + 4 + bodyLength) return;
                    const auto firstLine = QString::fromLatin1(header.left(header.indexOf("\r\n")));
                    requests.push_back(firstLine);
                    if (firstLine.startsWith("GET /v4/characters") && disconnectJikanResponses > 0) {
                        --disconnectJikanResponses;
                        socket->abort();
                        requestBytes->clear();
                        return;
                    }
                    QByteArray contentType = "application/json";
                    QByteArray statusLine = "HTTP/1.1 200 OK";
                    QByteArray response;
                    if (firstLine.contains("/image/")) {
                        if (image429Responses > 0) {
                            --image429Responses;
                            statusLine = "HTTP/1.1 429 Too Many Requests";
                            response = QByteArrayLiteral("{}");
                        } else {
                            contentType = wrongImageType ? QByteArrayLiteral("text/html") : QByteArrayLiteral("image/png");
                            response = oversizedImage ? QByteArray(5'000'001, 'x') : png;
                        }
                    } else if (firstLine.startsWith("POST /graphql")) {
                        ++aniListCalls;
                        response = aniListCalls == 1 ? QByteArrayLiteral(
                            "{\"data\":{\"Page\":{\"characters\":[{\"id\":77,\"name\":{\"full\":\"Blue Test\"},\"gender\":\"Female\",\"age\":\"17\",\"dateOfBirth\":{\"year\":2001,\"month\":2,\"day\":3},\"bloodType\":\"A\",\"favourites\":12,\"description\":\"Hair: blue\",\"image\":{\"large\":\"http://127.0.0.1:") +
                            QByteArray::number(server.serverPort()) + QByteArrayLiteral("/image/77\"},\"siteUrl\":\"https://anilist.co/character/77\"}]}}}")
                            : QByteArrayLiteral("{\"data\":{\"Page\":{\"characters\":[]}}}");
                    } else if (firstLine.startsWith("GET /v4/characters") || firstLine.startsWith("GET /v4/top/characters")) {
                        ++jikanCalls;
                        if (jikan429Responses > 0) {
                            --jikan429Responses;
                            statusLine = "HTTP/1.1 429 Too Many Requests";
                            response = QByteArrayLiteral("{}");
                        } else if (malformedJikan) response = QByteArrayLiteral("{bad json");
                        else if (jikanPagesServed++ > 0) response = QByteArrayLiteral("{\"data\":[]}");
                        else if (missingJikanImage) response = QByteArrayLiteral(
                            "{\"data\":[{\"mal_id\":89,\"name\":\"No Image\",\"images\":{\"jpg\":{}}}]}");
                        else response = QByteArrayLiteral(
                            "{\"data\":[{\"mal_id\":88,\"name\":\"Red Test\",\"url\":\"https://myanimelist.net/character/88\",\"favorites\":5,\"nicknames\":[\"R\"],\"about\":\"Age: 17\\nRed-haired girl\",\"images\":{\"jpg\":{\"image_url\":\"http://127.0.0.1:") +
                            QByteArray::number(server.serverPort()) + QByteArrayLiteral("/image/88\"}}}]}")
                            ;
                    } else {
                        response = QByteArrayLiteral("{\"data\":[]}");
                    }
                    const auto headers = statusLine + QByteArrayLiteral("\r\nContent-Type: ") + contentType +
                        (statusLine.contains("429") ? QByteArrayLiteral("\r\nRetry-After: 0") : QByteArray{}) +
                        QByteArrayLiteral("\r\nContent-Length: ") + QByteArray::number(response.size()) +
                        QByteArrayLiteral("\r\nConnection: close\r\n\r\n");
                    const auto sendResponse = [socket, headers, response] {
                        socket->write(headers + response);
                        socket->disconnectFromHost();
                    };
                    if (responseDelayMs > 0) QTimer::singleShot(responseDelayMs, socket, sendResponse);
                    else sendResponse();
                    requestBytes->clear();
                });
                QObject::connect(socket, &QObject::destroyed, &server, [requestBytes] { delete requestBytes; });
            }
        });
        check(server.listen(QHostAddress::LocalHost), "start local HTTP mock");
    }

    QUrl url(const QString& path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path)); }
};

void runAniList() {
    MockServer mock;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.aniList = true; options.jikan = false;
    QEventLoop loop;
    OnlineSearchProgress finalProgress;
    QStringList errors;
    search.start(options, {}, [&](const auto& p) { finalProgress = p; }, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(mock.aniListCalls >= 2, "AniList requests pages and stops at an empty page");
    check(mock.requests.first().startsWith("POST /graphql"), "AniList uses GraphQL POST");
    check(!mock.requests.isEmpty() && mock.requests.contains("GET /image/77 HTTP/1.1"), "AniList character image is fetched");
    const auto row = cache.get(-77);
    check(row.has_value(), "AniList character saved with negative cache id");
    check(row->character.name == "Blue Test" && row->character.gender == std::optional<std::string>("Female"), "AniList identity and gender stored");
    check(row->character.rgb.has_value() && row->character.rgb->at(2) > row->character.rgb->at(0), "downloaded image sent through hair detector");
    const auto info = QJsonDocument::fromJson(QByteArray::fromStdString(row->infoJson)).object();
    check(info.value("age").toString() == "17" && info.value("birthday").toString() == "Feb 3, 2001" &&
          info.value("blood").toString() == "A" && info.value("hair").toString() == "blue", "AniList and description metadata extracted");
    check(finalProgress.newlyAnalyzed == 1 && finalProgress.checked >= 1 && finalProgress.failed == 0, "AniList progress counts analysis");
    check(errors.isEmpty(), "AniList completes without reported API errors");
}

void runJikan() {
    MockServer mock;
    mock.jikan429Responses = 1;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 1;
    options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(mock.jikanCalls == 2, "Jikan 429 response retried once before success");
    check(mock.requests.first().startsWith("GET /v4/characters?q=Test&page=1&limit=25&order_by=favorites&sort=desc"),
          "Jikan name search query and ordering match Python behavior");
    const auto row = cache.get(88);
    check(row && row->character.name == "Red Test" && !row->character.gender, "Jikan MAL id and missing gender preserved");
    const auto info = QJsonDocument::fromJson(QByteArray::fromStdString(row->infoJson)).object();
    check(info.value("age").toString() == "17" && info.value("hair").toString() == "red" &&
          info.value("nicknames").toArray().size() == 1 && info.value("favs").toInt() == 5,
          "Jikan description and API metadata extracted");
    check(errors.isEmpty(), "Jikan completes without reported API errors");
}

void runJikanTopCharacters() {
    MockServer mock;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.target = 1; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    search.start(options, {}, {}, [&](const auto&) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(!mock.requests.isEmpty() && mock.requests.first().startsWith("GET /v4/top/characters?page=1&limit=25"),
          "Jikan no-query search uses its top characters endpoint");
    check(cache.get(88).has_value(), "Jikan top characters integrate with image analysis and cache");
}

void runConnectionRetry() {
    MockServer mock;
    mock.disconnectJikanResponses = 1;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 1;
    options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(mock.requests.size() >= 2 && mock.requests[0].startsWith("GET /v4/characters") &&
          mock.requests[1].startsWith("GET /v4/characters"), "Jikan disconnect is retried with a fresh request");
    check(cache.get(88).has_value() && errors.isEmpty(), "connection retry recovers and continues analysis");
}

void runMalformedApiResponse() {
    MockServer mock;
    mock.malformedJikan = true;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(errors.size() == 1 && errors.first().contains("malformed JSON"), "malformed Jikan JSON reported without crashing");
    check(!cache.get(88), "malformed API page does not create cache rows");
}

void runBadImageResponse() {
    MockServer mock;
    mock.wrongImageType = true;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 1; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    OnlineSearchProgress progress;
    search.start(options, {}, [&](const auto& p) { progress = p; }, [&](const auto&) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(progress.failed == 1 && progress.newlyAnalyzed == 0, "non-image response is rejected as failed character");
    check(!cache.get(88), "rejected image response is not cached");
}

void runImageRateLimitRetry() {
    MockServer mock;
    mock.image429Responses = 1;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 1;
    options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    OnlineSearchProgress progress;
    QStringList errors;
    search.start(options, {}, [&](const auto& p) { progress = p; }, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    const auto imageRequests = std::count_if(mock.requests.cbegin(), mock.requests.cend(),
        [](const QString& request) { return request.startsWith("GET /image/"); });
    check(imageRequests == 2 && progress.newlyAnalyzed == 1 && errors.isEmpty(),
          "JSON 429 image response follows Retry-After and retries before image content validation");
}

void runOversizedImage() {
    MockServer mock;
    mock.oversizedImage = true;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 1;
    options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    OnlineSearchProgress progress;
    search.start(options, {}, [&](const auto& p) { progress = p; }, [&](const auto&) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(progress.failed == 1 && !cache.get(88), "image above the Python 5 MB limit is rejected");
}

void runMissingImage() {
    MockServer mock;
    mock.missingJikanImage = true;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(mock.jikanCalls == 2 && errors.isEmpty(), "missing character image skips candidate and advances to next page");
}

void runCancellation() {
    MockServer mock;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    bool completed = false;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { completed = true; errors = e; });
    search.cancel();
    check(completed && errors.contains("Search stopped by user."), "cancel stops an active request and completes the search callback");
    check(mock.requests.isEmpty(), "cancelled request does not continue into API processing");
}

void runActiveCancellation() {
    MockServer mock;
    mock.responseDelayMs = 1000;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.jikan = true; options.aniList = false; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(100, &search, [&search] { search.cancel(); });
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    check(mock.requests.size() == 1 && mock.requests.first().startsWith("GET /v4/top/characters"),
          "active HTTP request reached the mock server before cancellation");
    check(errors.contains("Search stopped by user.") && !cache.get(88), "active request abort completes without caching its late response");
}

void runSourceFallback() {
    MockServer mock;
    mock.malformedJikan = true;
    Cache cache(":memory:");
    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.query = "Test"; options.target = 30; options.girlsOnly = false;
    QEventLoop loop;
    QStringList errors;
    search.start(options, {}, {}, [&](const auto& e) { errors = e; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    check(cache.get(-77).has_value(), "AniList search continues after Jikan fails");
    check(errors.size() == 1 && errors.first().startsWith("Jikan:"), "partial API failure is reported after fallback completes");
}

void runCacheBackfill() {
    MockServer mock;
    Cache cache(":memory:");
    CacheRecord old;
    old.character.id = -77; old.character.name = "Blue Test";
    old.character.rgb = Rgb{25, 80, 210}; old.character.confidence = .8;
    old.url = "https://anilist.co/character/77";
    old.imageUrl = mock.url("/image/77").toString().toStdString();
    old.imagePath = "old-thumbnail.jpg"; old.imageHash = "previous-hash"; old.infoJson = "{}";
    cache.save(old);

    OnlineSearch search(cache, mock.url("/v4/"), mock.url("/graphql"));
    OnlineSearchOptions options; options.aniList = true; options.jikan = false; options.target = 1;
    QEventLoop loop;
    OnlineSearchProgress progress;
    search.start(options, {}, [&](const auto& p) { progress = p; }, [&](const auto&) { loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    const auto row = cache.get(-77);
    check(row && row->character.gender == std::optional<std::string>("Female"), "cached AniList row backfills newly available gender");
    check(row && QJsonDocument::fromJson(QByteArray::fromStdString(row->infoJson)).object().value("birthday").toString() == "Feb 3, 2001",
          "cached row backfills metadata without downloading the same image again");
    check(!mock.requests.contains("GET /image/77 HTTP/1.1"), "matching cached image skips remote download");
    check(progress.newlyAnalyzed == 0 && progress.checked == 1, "cache hit counts as checked but not newly analyzed");
}

void runStaleReanalysis() {
    QTemporaryDir temp;
    check(temp.isValid(), "create stale cache test directory");
    const auto imagePath = temp.filePath("cached.jpg");
    QImage image(64, 64, QImage::Format_RGB32);
    image.fill(qRgb(245, 245, 245));
    for (int y = 0; y < 31; ++y) for (int x = 8; x < 56; ++x) image.setPixel(x, y, qRgb(25, 80, 210));
    check(image.save(imagePath, "JPEG"), "write stale cached thumbnail");
    const auto dbPath = temp.filePath("cache.db");
    Cache cache(dbPath.toStdString());
    CacheRecord old;
    old.character.id = 177; old.character.name = "Stale analysis";
    old.url = "https://example.test/177"; old.imageUrl = "https://example.test/image/177";
    old.imagePath = imagePath.toStdString(); old.imageHash = "old-hash"; old.infoJson = "{\"about\":\"kept\"}";
    cache.save(old);
    const auto before = cache.get(177);
    sqlite3* db = nullptr;
    check(sqlite3_open(dbPath.toStdString().c_str(), &db) == SQLITE_OK, "open stale cache fixture");
    check(sqlite3_exec(db, "UPDATE chars SET version=1 WHERE mal_id=177", nullptr, nullptr, nullptr) == SQLITE_OK,
          "mark cached detector result stale");
    sqlite3_close(db);

    OnlineSearch search(cache);
    QEventLoop loop;
    int updated = 0;
    QString error;
    search.reanalyzeStale([&](int count, const QString& message) { updated = count; error = message; loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    const auto after = cache.get(177);
    check(updated == 1 && error.isEmpty(), "stale analyses are reprocessed without API access");
    check(after && after->character.version == 2 && after->character.rgb.has_value(), "stale detector result and version updated");
    check(after && after->character.timestamp == before->character.timestamp && after->infoJson == old.infoJson,
          "offline reanalysis preserves timestamp and metadata");
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        runAniList();
        runJikan();
        runJikanTopCharacters();
        runConnectionRetry();
        runMalformedApiResponse();
        runBadImageResponse();
        runImageRateLimitRetry();
        runOversizedImage();
        runMissingImage();
        runCancellation();
        runActiveCancellation();
        runSourceFallback();
        runCacheBackfill();
        runStaleReanalysis();
        std::cout << "Passed mocked AniList/Jikan search, image, cache, and metadata checks\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}
