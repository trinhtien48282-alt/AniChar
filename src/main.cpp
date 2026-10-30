#include "anichar/cache.hpp"
#include "anichar/hair_detector.hpp"
#include "anichar/online_search.hpp"

#include <QApplication>
#include <QColorDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressBar>
#include <QPixmap>
#include <QScrollArea>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <stdexcept>

using namespace anichar;

namespace {
class MainWindow final : public QMainWindow {
public:
    MainWindow() {
        const auto cachePath = QDir::homePath() + QStringLiteral("/.anime_hair_finder/cache.db");
        cache_ = std::make_unique<Cache>(cachePath.toStdString());
        online_ = std::make_unique<OnlineSearch>(*cache_, this);

        auto* central = new QWidget;
        auto* layout = new QVBoxLayout(central);
        auto* form = new QFormLayout;
        auto* colorRow = new QHBoxLayout;
        color_ = new QLineEdit(QStringLiteral("#6BB6D9"));
        auto* choose = new QPushButton(QStringLiteral("Pick color"));
        colorRow->addWidget(color_);
        colorRow->addWidget(choose);
        form->addRow(QStringLiteral("Target RGB"), colorRow);
        colorInfo_ = new QLabel;
        form->addRow(QStringLiteral("Color values"), colorInfo_);
        query_ = new QLineEdit;
        query_->setPlaceholderText(QStringLiteral("Character name (optional)"));
        form->addRow(QStringLiteral("Name"), query_);
        girlsOnly_ = new QCheckBox(QStringLiteral("Girls only (AniList provides gender)"));
        girlsOnly_->setChecked(true);
        form->addRow(QStringLiteral("Filter"), girlsOnly_);
        rejectConflicts_ = new QCheckBox(QStringLiteral("Hide listed-hair conflicts"));
        rejectConflicts_->setChecked(true);
        form->addRow(QStringLiteral("Filter"), rejectConflicts_);
        jikan_ = new QCheckBox(QStringLiteral("Jikan (MyAnimeList)"));
        jikan_->setChecked(true);
        form->addRow(QStringLiteral("Source"), jikan_);
        aniList_ = new QCheckBox(QStringLiteral("AniList"));
        aniList_->setChecked(true);
        form->addRow(QStringLiteral("Source"), aniList_);
        depth_ = new QComboBox;
        depth_->addItems({QStringLiteral("30 new analyses"), QStringLiteral("80 new analyses"), QStringLiteral("200 new analyses")});
        form->addRow(QStringLiteral("Online search depth"), depth_);
        tolerance_ = new QSlider(Qt::Horizontal);
        tolerance_->setRange(3, 50);
        tolerance_->setValue(25);
        form->addRow(QStringLiteral("Maximum color distance"), tolerance_);
        confidence_ = new QSlider(Qt::Horizontal);
        confidence_->setRange(0, 90);
        form->addRow(QStringLiteral("Minimum confidence (%)"), confidence_);
        sort_ = new QComboBox;
        sort_->addItems({QStringLiteral("Best match"), QStringLiteral("Highest confidence"),
                         QStringLiteral("Name"), QStringLiteral("Newest analysis")});
        form->addRow(QStringLiteral("Sort"), sort_);
        layout->addLayout(form);

        auto* buttons = new QHBoxLayout;
        auto* browse = new QPushButton(QStringLiteral("Analyze local image"));
        auto* refresh = new QPushButton(QStringLiteral("Refresh cached results"));
        searchButton_ = new QPushButton(QStringLiteral("Search Jikan + AniList"));
        stopButton_ = new QPushButton(QStringLiteral("Stop"));
        stopButton_->setEnabled(false);
        buttons->addWidget(browse);
        buttons->addWidget(refresh);
        buttons->addWidget(searchButton_);
        buttons->addWidget(stopButton_);
        layout->addLayout(buttons);
        status_ = new QLabel(QStringLiteral("Choose a target color or analyze an image."));
        layout->addWidget(status_);
        progress_ = new QProgressBar;
        progress_->setRange(0, 30);
        progress_->setValue(0);
        layout->addWidget(progress_);
        results_ = new QVBoxLayout;
        auto* resultWidget = new QWidget;
        resultWidget->setLayout(results_);
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setWidget(resultWidget);
        layout->addWidget(scroll, 1);
        setCentralWidget(central);
        setWindowTitle(QStringLiteral("AniChar — Anime Hair Color Finder"));
        resize(850, 680);

        connect(choose, &QPushButton::clicked, this, [this] { pickColor(); });
        connect(browse, &QPushButton::clicked, this, [this] { analyzeLocal(); });
        connect(refresh, &QPushButton::clicked, this, [this] { refreshResults(); });
        connect(searchButton_, &QPushButton::clicked, this, [this] { startSearch(); });
        connect(stopButton_, &QPushButton::clicked, this, [this] { online_->cancel(); });
        connect(color_, &QLineEdit::returnPressed, this, [this] { refreshResults(); });
        connect(query_, &QLineEdit::returnPressed, this, [this] { refreshResults(); });
        connect(girlsOnly_, &QCheckBox::toggled, this, [this] { refreshResults(); });
        connect(rejectConflicts_, &QCheckBox::toggled, this, [this] { refreshResults(); });
        connect(tolerance_, &QSlider::valueChanged, this, [this] { refreshResults(); });
        connect(confidence_, &QSlider::valueChanged, this, [this] { refreshResults(); });
        connect(sort_, &QComboBox::currentIndexChanged, this, [this] { refreshResults(); });
        refreshResults();
        online_->reanalyzeStale([this](int count, const QString& error) {
            refreshResults();
            if (!error.isEmpty()) status_->setText(QStringLiteral("Could not update cached analyses: %1").arg(error));
            else if (count > 0 && !searchRunning_)
                status_->setText(QStringLiteral("Updated %1 cached analysis/analyses from saved images.").arg(count));
        });
    }

private:
    void startSearch() {
        try { (void)target(); }
        catch (const std::exception& e) { showError(QStringLiteral("Invalid target color: ") + QString::fromUtf8(e.what())); return; }
        OnlineSearchOptions options;
        options.query = query_->text().trimmed();
        options.target = depth_->currentIndex() == 0 ? 30 : depth_->currentIndex() == 1 ? 80 : 200;
        options.girlsOnly = girlsOnly_->isChecked();
        options.jikan = jikan_->isChecked();
        options.aniList = aniList_->isChecked();
        searchButton_->setEnabled(false);
        stopButton_->setEnabled(true);
        searchRunning_ = true;
        latestProgress_ = {};
        progress_->setRange(0, options.target);
        progress_->setValue(0);
        online_->start(options,
            [this](const QString& text) { status_->setText(text); },
            [this, target = options.target](const OnlineSearchProgress& p) {
                progress_->setValue(std::min(p.newlyAnalyzed, target));
                status_->setText(QStringLiteral("New: %1 / %2    Checked: %3    Failed: %4")
                    .arg(p.newlyAnalyzed).arg(target).arg(p.checked).arg(p.failed));
                latestProgress_ = p;
                if (!refreshQueued_) {
                    refreshQueued_ = true;
                    QTimer::singleShot(250, this, [this] {
                        refreshQueued_ = false;
                        if (searchRunning_) refreshResults();
                    });
                }
            },
            [this](const QStringList& errors) {
                searchRunning_ = false;
                searchButton_->setEnabled(true);
                stopButton_->setEnabled(false);
                refreshResults();
                const auto summary = QStringLiteral("%1 new, %2 checked, %3 failed.")
                    .arg(latestProgress_.newlyAnalyzed).arg(latestProgress_.checked).arg(latestProgress_.failed);
                if (!errors.isEmpty()) status_->setText(summary + " " + errors.join("; "));
                else status_->setText(QStringLiteral("Search finished. ") + summary);
            });
    }

    Rgb target() const { return parseColor(color_->text().toStdString()); }

    void pickColor() {
        try {
            const auto rgb = target();
            const auto chosen = QColorDialog::getColor(QColor(rgb[0], rgb[1], rgb[2]), this,
                                                       QStringLiteral("Target hair color"));
            if (chosen.isValid()) {
                color_->setText(QStringLiteral("#%1%2%3").arg(chosen.red(), 2, 16, QLatin1Char('0'))
                    .arg(chosen.green(), 2, 16, QLatin1Char('0')).arg(chosen.blue(), 2, 16, QLatin1Char('0')).toUpper());
                refreshResults();
            }
        } catch (const std::exception& e) { showError(QString::fromUtf8(e.what())); }
    }

    void analyzeLocal() {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Analyze image"), {},
            QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif *.tif *.tiff);;All files (*)"));
        if (path.isEmpty()) return;
        try {
            const auto detected = detectHair(loadImage(path));
            if (!detected) {
                status_->setText(QStringLiteral("Hair color not detected. Try a clear portrait image."));
                return;
            }
            const auto rgb = detected->rgb;
            const auto distance = deltaE(toLab(rgb), toLab(target()));
            status_->setText(QStringLiteral("Detected #%1%2%3 (%4); target distance %5, match %6%, confidence %7%")
                .arg(rgb[0], 2, 16, QLatin1Char('0')).arg(rgb[1], 2, 16, QLatin1Char('0'))
                .arg(rgb[2], 2, 16, QLatin1Char('0')).arg(QString::fromStdString(colorName(rgb)))
                .arg(distance, 0, 'f', 1).arg(similarity(distance), 0, 'f', 1)
                .arg(detected->confidence * 100, 0, 'f', 0));
        } catch (const std::exception& e) { showError(QStringLiteral("Could not read image: ") + QString::fromUtf8(e.what())); }
    }

    void refreshResults() {
        try {
            RankOptions options;
            options.target = target();
            const auto hsv = toHsv(options.target);
            const auto hsl = toHsl(options.target);
            colorInfo_->setText(QStringLiteral("RGB %1, %2, %3    HSV %4°, %5%, %6%    HSL %7°, %8%, %9%")
                .arg(options.target[0]).arg(options.target[1]).arg(options.target[2])
                .arg(hsv.hue).arg(hsv.saturation).arg(hsv.value)
                .arg(hsl.hue).arg(hsl.saturation).arg(hsl.lightness));
            options.girlsOnly = girlsOnly_->isChecked();
            options.rejectHairConflicts = rejectConflicts_->isChecked();
            options.maxDistance = tolerance_->value();
            options.minConfidence = confidence_->value() / 100.0;
            options.nameQuery = query_->text().toStdString();
            options.sort = static_cast<SortOrder>(sort_->currentIndex());
            const auto rows = cache_->ranked(options);
            while (auto* item = results_->takeAt(0)) {
                delete item->widget();
                delete item;
            }
            for (const auto& row : rows) {
                const auto& character = row.record.character;
                auto* card = new QWidget;
                auto* cardLayout = new QHBoxLayout(card);
                auto* image = new QLabel;
                const auto imagePath = QString::fromStdString(row.record.imagePath);
                if (QFileInfo::exists(imagePath))
                    image->setPixmap(QPixmap(imagePath).scaled(100, 140, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                image->setFixedSize(104, 144);
                cardLayout->addWidget(image);
                auto* label = new QLabel;
                label->setWordWrap(true);
                label->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
                const auto info = QJsonDocument::fromJson(QByteArray::fromStdString(row.record.infoJson)).object();
                QStringList lines;
                const auto name = QString::fromStdString(character.name).toHtmlEscaped();
                const auto url = QString::fromStdString(row.record.url).toHtmlEscaped();
                lines << QStringLiteral("<a href=\"%1\"><b>%2</b></a>   %3% match")
                    .arg(url, name).arg(row.similarity, 0, 'f', 1);
                QString detectedHair = QStringLiteral("App sees hair as: %1 (%2)")
                    .arg(QString::fromStdString(colorName(*character.rgb)), QString::fromStdString(toHex(*character.rgb)));
                if (info.value("hair").isString() && !info.value("hair").toString().isEmpty()) {
                    const auto listed = info.value("hair").toString();
                    detectedHair += QStringLiteral("<br>Listed hair: %1%2")
                        .arg(listed.toHtmlEscaped(), hairConflict(*character.rgb, listed.toStdString()) ? QStringLiteral("  (!) differs") : QString{});
                }
                lines << detectedHair;
                lines << QStringLiteral("Target %1   dE %2<br>Analysis confidence %3%%%4")
                    .arg(QString::fromStdString(toHex(options.target)))
                    .arg(row.distance, 0, 'f', 1).arg(*character.confidence * 100, 0, 'f', 0)
                    .arg(*character.confidence < .5 ? QStringLiteral("  (!) low") : QString{});
                QStringList facts;
                for (const auto& key : {QStringLiteral("age"), QStringLiteral("birthday"), QStringLiteral("blood")})
                    if (info.value(key).isString() && !info.value(key).toString().isEmpty())
                        facts << QStringLiteral("%1: %2").arg(key, info.value(key).toString().toHtmlEscaped());
                if (character.gender) facts << QString::fromStdString(*character.gender).toHtmlEscaped();
                if (info.value("favs").isDouble()) facts << QStringLiteral("%1 favorites").arg(info.value("favs").toInt());
                const auto nicknames = info.value("nicknames").toArray();
                if (!nicknames.isEmpty()) {
                    QStringList names;
                    for (const auto& nickname : nicknames) names << nickname.toString().toHtmlEscaped();
                    facts << QStringLiteral("aka %1").arg(names.join(", "));
                }
                if (!facts.isEmpty()) lines << facts.join("   ");
                if (info.value("about").isString() && !info.value("about").toString().isEmpty())
                    lines << info.value("about").toString().toHtmlEscaped();
                label->setText(lines.join("<br>"));
                connect(label, &QLabel::linkActivated, this, [](const QString& link) {
                    const QUrl url(link);
                    if (url.scheme() == "http" || url.scheme() == "https") QDesktopServices::openUrl(url);
                });
                cardLayout->addWidget(label, 1);
                card->setMinimumHeight(152);
                results_->addWidget(card);
            }
            results_->addStretch();
            if (!searchRunning_) status_->setText(QStringLiteral("%1 cached match(es).").arg(rows.size()));
        } catch (const std::exception& e) { showError(QString::fromUtf8(e.what())); }
    }

    void showError(const QString& message) {
        QMessageBox::warning(this, QStringLiteral("AniChar"), message);
        status_->setText(message);
    }

    std::unique_ptr<Cache> cache_;
    std::unique_ptr<OnlineSearch> online_;
    QLineEdit* color_ = nullptr;
    QLineEdit* query_ = nullptr;
    QSlider* tolerance_ = nullptr;
    QSlider* confidence_ = nullptr;
    QComboBox* sort_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* colorInfo_ = nullptr;
    QVBoxLayout* results_ = nullptr;
    QCheckBox* girlsOnly_ = nullptr;
    QCheckBox* rejectConflicts_ = nullptr;
    QCheckBox* jikan_ = nullptr;
    QCheckBox* aniList_ = nullptr;
    QComboBox* depth_ = nullptr;
    QPushButton* searchButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QProgressBar* progress_ = nullptr;
    OnlineSearchProgress latestProgress_;
    bool refreshQueued_ = false;
    bool searchRunning_ = false;
};
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("AniChar"));
    try {
        MainWindow window;
        window.show();
        return app.exec();
    } catch (const std::exception& e) {
        QMessageBox::critical(nullptr, QStringLiteral("AniChar startup error"), QString::fromUtf8(e.what()));
        return 1;
    }
}
