#pragma once

#include <QObject>
#include <QUrl>

#include <functional>
#include <memory>

namespace anichar {
class Cache;

struct OnlineSearchOptions {
    QString query;
    int target = 30;
    bool girlsOnly = true;
    bool jikan = true;
    bool aniList = true;
};

struct OnlineSearchProgress {
    int newlyAnalyzed = 0;
    int checked = 0;
    int failed = 0;
};

class OnlineSearch final : public QObject {
public:
    using Status = std::function<void(const QString&)>;
    using Progress = std::function<void(const OnlineSearchProgress&)>;
    using Finished = std::function<void(const QStringList&)>;
    using ReanalysisFinished = std::function<void(int, const QString&)>;

    explicit OnlineSearch(Cache& cache, QObject* parent = nullptr);
    OnlineSearch(Cache& cache, QUrl jikanBase, QUrl aniListUrl, QObject* parent = nullptr);
    ~OnlineSearch() override;
    void start(const OnlineSearchOptions& options, Status status, Progress progress, Finished finished);
    void cancel();
    void reanalyzeStale(ReanalysisFinished finished);

    static QUrl jikanBaseUrl();
    static QUrl aniListUrl();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
