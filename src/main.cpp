#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QNetworkAccessManager>
#include <QString>
#include <sqlite3.h>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QNetworkAccessManager network;

    QMainWindow window;
    auto* label = new QLabel(
        QStringLiteral("AniChar Qt dependency smoke test\nQt %1\nSQLite %2\nNetwork: %3")
            .arg(QString::fromLatin1(qVersion()),
                 QString::fromUtf8(sqlite3_libversion()),
                 QString::fromLatin1(network.metaObject()->className())));
    label->setMargin(24);
    window.setCentralWidget(label);
    window.setWindowTitle(QStringLiteral("AniChar Qt Smoke Test"));
    window.resize(420, 180);
    window.show();

    return app.exec();
}
