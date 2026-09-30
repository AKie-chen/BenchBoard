#include "MainWindow.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QString mode   = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("normal");

    MainWindow w;
    w.resize(900, 620);
    w.show();

    QTimer::singleShot(600, [&] {
        QPushButton *start = nullptr;
        for (QPushButton *b : w.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("开始"))) start = b;
        }
        if (!start) { std::printf("RESULT=NO_START_BUTTON\n"); app.quit(); return; }

        if (QSpinBox *sp = w.findChild<QSpinBox *>()) sp->setValue(5);

        std::printf("click start (enabled=%d)\n", int(start->isEnabled()));
        start->click();
        std::printf("after click: start.enabled=%d\n", int(start->isEnabled()));
    });

    if (mode == QStringLiteral("stop")) {
        QTimer::singleShot(4000, [&] {
            QPushButton *stop = nullptr;
            for (QPushButton *b : w.findChildren<QPushButton *>()) {
                if (b->text().contains(QStringLiteral("停止"))) stop = b;
            }
            if (!stop) { std::printf("RESULT=NO_STOP_BUTTON\n"); return; }
            std::printf("click stop (enabled=%d)\n", int(stop->isEnabled()));
            stop->click();
        });
    }

    QTimer::singleShot(14000, [&] {
        QPlainTextEdit *log = w.findChild<QPlainTextEdit *>();
        const QString content = log ? log->toPlainText() : QString();

        QFile f(outDir + "/log_dump.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts << content;
            f.close();
            std::printf("LOG_WRITTEN=%s\n", qPrintable(outDir + "/log_dump.txt"));
        }

        std::printf("LOG_CHARS=%lld\n", (long long)content.size());
        std::printf("HAS_JSON_http_req_duration=%d\n",
                    int(content.contains(QStringLiteral("http_req_duration"))));
        std::printf("HAS_JSON_TYPE_Point=%d\n",
                    int(content.contains(QStringLiteral("\"type\""))));
        std::printf("HAS_PROGRESS=%d\n",
                    int(content.contains(QStringLiteral("已接收"))));
        std::printf("HAS_ENGINE_END=%d\n",
                    int(content.contains(QStringLiteral("引擎已结束"))));
        std::printf("HAS_CRASH_MARK=%d\n",
                    int(content.contains(QStringLiteral("被强制结束"))));

        if (log) {
            const QStringList head = content.split(QChar('\n')).mid(0, 6);
            std::printf("--- first 6 lines ---\n");
            for (const QString &l : head) std::printf("  | %s\n", qPrintable(l.left(110)));
        }

        std::printf("MODE=%s\n", qPrintable(mode));
        for (QPushButton *b : w.findChildren<QPushButton *>()) {
            std::printf("BTN  %-12s enabled=%d\n", qPrintable(b->text()), int(b->isEnabled()));
        }
        for (QLabel *l : w.findChildren<QLabel *>()) {
            if (!l->text().isEmpty())
                std::printf("LABEL %-12s = %s\n", qPrintable(l->objectName()), qPrintable(l->text()));
        }

        w.grab().save(outDir + "/m2_e2e.png");
        app.quit();
    });

    return app.exec();
}
