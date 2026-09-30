// ============================================================================
// 火管 UI 探针 —— 客观度量「k6 早就跑完了，界面还要多久才承认」
//
// 为什么需要它：
//   m3_e2e_probe 的判据全是「数据对不对」（点数单调不减、点数 > 0）。
//   它【不度量时间】—— 所以数据再正确，界面卡死也照样全绿。
//   本探针把度量对象换成【时间】和【事件循环的可响应性】。
//
// 三个独立指标：
//   ① 心跳间隔 —— 挂一个 100ms 的 QTimer。事件循环只要被 readyRead 回调独占，
//      这个心跳就会被推迟。最大间隔 = 界面最长一次"没反应"的时间。
//   ② k6 真实退出时刻 —— 用 reports/summary.json 的 mtime。这是【外部证据】，
//      不受界面状态影响。k6 一写完 summary 就退出。
//   ③ 界面承认结束的时刻 —— 轮询日志区出现"引擎已结束"那一行。
//   ②③ 之差 = 界面滞后，也就是你感受到的"10 秒的测试跑了快一分钟"。
//
// 用法（见 firehose_ui_probe_run.sh）：
//   firehose_ui_probe.exe <输出目录> <目标URL> <VU数> <时长(秒)> [上限(秒)]
// ============================================================================

#include "MainWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <algorithm>
#include <cstdio>

struct Tick { qint64 atMs; qint64 gapMs; };

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QString url    = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("http://127.0.0.1:8917/");
    const int     vus    = (argc > 3) ? std::atoi(argv[3]) : 8;
    const int     durSec = (argc > 4) ? std::atoi(argv[4]) : 10;
    const qint64  capSec = (argc > 5) ? std::atoi(argv[5]) : 180;

    MainWindow w;
    w.resize(940, 620);
    w.show();

    QElapsedTimer wall;
    wall.start();

    QVector<Tick> ticks;
    qint64 lastTick = 0;

    // ---- 指标① 100ms 心跳：度量事件循环被独占多久 ----
    QTimer beat;
    beat.setInterval(100);
    QObject::connect(&beat, &QTimer::timeout, [&] {
        const qint64 now = wall.elapsed();
        ticks.append(Tick{now, now - lastTick});
        lastTick = now;
    });

    bool  started   = false;
    qint64 tStartMs = 0;
    bool  done      = false;
    qint64 tUiDone  = 0;

    QTimer::singleShot(600, [&] {
        if (QLineEdit *urlEdit = w.findChild<QLineEdit *>()) urlEdit->setText(url);
        if (QSpinBox *sp = w.findChild<QSpinBox *>())        sp->setValue(vus);
        if (QComboBox *cb = w.findChild<QComboBox *>()) {
            // 第一项就是「10 秒」；要找别的时长就 index = 0/1/2
            const int idx = (durSec <= 10) ? 0 : (durSec <= 30 ? 1 : 2);
            cb->setCurrentIndex(idx);
            std::printf("duration combo idx=%d text=%s data=%s\n", idx,
                        qPrintable(cb->currentText()), qPrintable(cb->currentData().toString()));
        }

        QPushButton *start = nullptr;
        for (QPushButton *b : w.findChildren<QPushButton *>())
            if (b->text().contains(QStringLiteral("开始"))) start = b;
        if (!start) { std::printf("RESULT=NO_START_BUTTON\n"); app.quit(); return; }

        beat.start();
        start->click();
        started  = true;
        tStartMs = wall.elapsed();
        std::printf("t=%lldms  已点开始  目标=%s  VU=%d  时长=%ds\n",
                    (long long)tStartMs, qPrintable(url), vus, durSec);
        std::fflush(stdout);
    });

    // ---- 指标③ 轮询日志区，看界面什么时候才把"结束"渲染出来 ----
    QTimer poll;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (!started || done) return;
        QPlainTextEdit *log = w.findChild<QPlainTextEdit *>();
        if (log && log->toPlainText().contains(QStringLiteral("引擎已结束"))) {
            done     = true;
            tUiDone  = wall.elapsed();
            std::printf("t=%lldms  日志区出现「引擎已结束」\n", (long long)tUiDone);
            std::fflush(stdout);
        }
    });

    const qint64 capMs = capSec * 1000;

    // ---- 收尾：由轮询发现完成后再等 1 秒落盘 ----
    QTimer finishWatch;
    finishWatch.setInterval(200);
    QObject::connect(&finishWatch, &QTimer::timeout, [&] {
        if (!started) return;
        if (!done && wall.elapsed() < capMs) return;
        if (done && wall.elapsed() < tUiDone + 1000) return;
        finishWatch.stop();
        beat.stop();
        poll.stop();

        const qint64 endMs = wall.elapsed();

        // ---- 指标② k6 真实退出时刻：summary.json 的 mtime（外部时钟）----
        const QFileInfo fi(QStringLiteral(BENCHBOARD_ROOT "/reports/summary.json"));
        qint64 k6DoneMs = -1;
        if (fi.exists()) {
            const qint64 mtimeEpoch = fi.lastModified().toMSecsSinceEpoch();
            // 把 epoch 换算到"相对本次 wall 起点"的毫秒
            const qint64 wallStartEpoch = QDateTime::currentMSecsSinceEpoch() - endMs;
            k6DoneMs = mtimeEpoch - wallStartEpoch;
        }

        std::printf("\n=== 时间线（相对「点开始」）===\n");
        std::printf("  k6 真实退出(summary.json mtime) : %lld ms\n", (long long)k6DoneMs);
        std::printf("  界面承认结束(日志出现结束行)     : %lld ms\n", (long long)tUiDone);
        if (k6DoneMs > 0 && tUiDone > 0) {
            std::printf("  ★ 界面滞后                      : %lld ms  (%.1f 秒)\n",
                        (long long)(tUiDone - k6DoneMs), (tUiDone - k6DoneMs) / 1000.0);
        }
        std::printf("  探针总墙钟                      : %lld ms\n", (long long)endMs);

        // ---- 心跳统计 = 事件循环可响应性 ----
        QVector<qint64> gaps;
        for (const Tick &t : ticks) if (t.gapMs > 0) gaps.append(t.gapMs);
        std::sort(gaps.begin(), gaps.end());
        qint64 maxGap = gaps.isEmpty() ? 0 : gaps.last();
        qint64 p50 = 0, p95 = 0;
        if (!gaps.isEmpty()) {
            p50 = gaps[gaps.size() / 2];
            p95 = gaps[qMin<qsizetype>(gaps.size() - 1, gaps.size() * 95 / 100)];
        }
        int over500 = 0, over2000 = 0;
        for (qint64 g : gaps) { if (g > 500) ++over500; if (g > 2000) ++over2000; }

        std::printf("\n=== 事件循环心跳（名义 100ms 一次，共 %lld 次）===\n",
                    (long long)ticks.size());
        std::printf("  间隔 p50 = %lld ms   p95 = %lld ms   最大 = %lld ms\n",
                    (long long)p50, (long long)p95, (long long)maxGap);
        std::printf("  间隔 > 500ms 的次数 = %d ； > 2000ms 的次数 = %d\n", over500, over2000);
        std::printf("  ★ 最大间隔就是界面最长一次「点什么都没反应」的时长\n");

        // ---- 日志落盘 ----
        QPlainTextEdit *log = w.findChild<QPlainTextEdit *>();
        const QString content = log ? log->toPlainText() : QString();
        {
            QFile f(outDir + "/firehose_log.txt");
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                QTextStream ts(&f);
                ts.setEncoding(QStringConverter::Utf8);
                ts << content;
            }
        }
        std::printf("\nLOG_CHARS=%lld\n", (long long)content.size());
        w.grab().save(outDir + "/firehose_ui.png");
        std::printf("RESULT=DONE\n");
        std::fflush(stdout);
        app.quit();
    });
    finishWatch.start();

    return app.exec();
}
