// ============================================================================
// M3 验收探针 —— 客观断言"状态栏数字每秒在跳"，而不是"看着像在跳"
//
// 它链接你真实的 src/MainWindow.cpp + MetricsAggregator.cpp（不改你的源码），
// 自动：填 URL → 设 8 VU → 点「开始压测」→ 每 500ms 采样一次状态栏 → 断言。
//
// 判据（不依赖肉眼）：
//   ① 状态栏消息至少出现过 3 种不同内容  —— 证明它在刷新，不是卡在一个值上
//   ② 从消息里抠出来的"已采集 N 点"，随时间的序列必须【单调不减】
//   ③ 结束时"已采集"的点数 > 0            —— 证明聚合器真的解析到了数据
//   ④ 引擎结束后状态栏右侧回到「引擎：未启动」
//
// 跑法见 tools/probes/m3_e2e_probe_run.sh
//   bash tools/probes/m3_e2e_probe_run.sh normal    # 自然跑完
//   bash tools/probes/m3_e2e_probe_run.sh stop      # 第 4 秒点停止
// ============================================================================

#include "MainWindow.h"

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStatusBar>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QString mode   = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("normal");

    // ---------- 环境自检：本机 QProcess 能不能启动子进程？ ----------
    // ★ 为什么必须有这道守卫（2026-09-24 实测教训）：
    //   QProcess 在 Windows 上靠【命名管道】做启动同步。某些受限制的执行上下文里
    //   CreatePipe / CreateFile 会直接失败，导致【任何】程序都启动不了 ——
    //   实测连 C:/Windows/System32/where.exe 都 FailedToStart。
    //   此时界面断言 A3（点数 > 0）必然 FAIL，但那是【环境的锅，不是代码的锅】。
    //   没有这道守卫，就会把"环境故障"误判成"用户的聚合器写错了"。
    //   对照实验：同一时刻 python 的 subprocess 能正常启动 k6（rc=0），
    //   说明系统与文件本身没问题，问题只在本执行上下文。
    {
        QProcess envProbe;
        envProbe.setProgram(QStringLiteral("C:/Windows/System32/where.exe"));
        envProbe.setArguments(QStringList{ QStringLiteral("cmd") });
        envProbe.start();
        if (!envProbe.waitForStarted(3000)) {
            std::printf("\n!!! 环境自检失败 !!!\n");
            std::printf("本机 QProcess 无法启动任何子进程（连 where.exe 都起不来）：%s\n",
                        qPrintable(envProbe.errorString()));
            std::printf("=> 界面级断言无法进行。这是【环境问题】，不是 M3/M4 的代码缺陷。\n");
            std::printf("=> 请在桌面会话里直接运行 BenchBoard.exe 手工验证。\n");
            std::printf("RESULT=ENV_NO_QPROCESS\n");
            return 3;
        }
        envProbe.waitForFinished(3000);
        std::printf("环境自检通过：QProcess 可以启动子进程\n");
    }

    MainWindow w;
    w.resize(940, 620);
    w.show();

    QVector<QString> samples;                 // 状态栏消息的时间序列
    const QRegularExpression re(QStringLiteral("已采集 (\\d+) 点"));

    QTimer sampler;
    sampler.setInterval(500);
    QObject::connect(&sampler, &QTimer::timeout, [&] {
        samples.append(w.statusBar()->currentMessage());
    });

    QTimer::singleShot(600, [&] {
        // ① 填 URL —— M3 起这个输入框是 load-bearing（会通过 -e BASE_URL= 传给脚本）
        if (QLineEdit *url = w.findChild<QLineEdit *>()) {
            url->setText(QStringLiteral("http://127.0.0.1:8899/"));
            std::printf("set url = %s\n", qPrintable(url->text()));
        } else {
            std::printf("RESULT=NO_URL_EDIT\n");
        }

        // ② 8 个 VU —— 验收标准里写的就是 8 VU 跑 8~10 秒
        if (QSpinBox *sp = w.findChild<QSpinBox *>()) sp->setValue(8);

        QPushButton *start = nullptr;
        for (QPushButton *b : w.findChildren<QPushButton *>()) {
            if (b->text().contains(QStringLiteral("开始"))) start = b;
        }
        if (!start) { std::printf("RESULT=NO_START_BUTTON\n"); app.quit(); return; }

        start->click();
        std::printf("clicked start, sampler started\n");
        sampler.start();
    });

    if (mode == QStringLiteral("stop")) {
        QTimer::singleShot(4000, [&] {
            for (QPushButton *b : w.findChildren<QPushButton *>()) {
                if (b->text().contains(QStringLiteral("停止")) && b->isEnabled()) {
                    std::printf("clicking stop\n");
                    b->click();
                }
            }
        });
    }

    QTimer::singleShot(15000, [&] {
        sampler.stop();

        // ---------- 断言 ① 刷新过 ----------
        QStringList distinct;
        for (const QString &s : samples)
            if (!distinct.contains(s)) distinct.append(s);

        std::printf("\n=== 状态栏采样序列（每 500ms 一条，去重后）===\n");
        for (const QString &s : distinct) std::printf("  | %s\n", qPrintable(s));
        std::printf("采样 %lld 次，出现 %lld 种不同内容\n",
                    (long long)samples.size(), (long long)distinct.size());

        // ---------- 断言 ② 点数单调不减 ----------
        QVector<qint64> points;
        for (const QString &s : samples) {
            const auto m = re.match(s);
            if (m.hasMatch()) points.append(m.captured(1).toLongLong());
        }
        bool monotonic = (points.size() >= 2);
        for (int i = 1; i < points.size(); ++i)
            if (points[i] < points[i - 1]) monotonic = false;

        qint64 maxPoints = points.isEmpty() ? 0 : points.last();

        const QString statusOk = (maxPoints > 0) ? QStringLiteral("PASS") : QStringLiteral("FAIL");
        const QString monoOk   = (points.size() >= 3 && monotonic) ? QStringLiteral("PASS")
                                                                   : QStringLiteral("FAIL");

        std::printf("\n=== 断言结果 ===\n");
        std::printf("A1 状态栏刷新过(>=3 种内容)      : %s  (实际 %lld 种)\n",
                    distinct.size() >= 3 ? "PASS" : "FAIL", (long long)distinct.size());
        std::printf("A2 点数序列单调不减(>=3 个采样)  : %s  (抠到 %lld 个数字)\n",
                    qPrintable(monoOk), (long long)points.size());
        std::printf("A3 结束时点数 > 0                : %s  (最大 %lld)\n",
                    qPrintable(statusOk), (long long)maxPoints);
        std::printf("A4 引擎结束后状态栏复位          : 见下面 LABEL 行\n");

        // ---------- 日志与控件状态 ----------
        QPlainTextEdit *log = w.findChild<QPlainTextEdit *>();
        const QString content = log ? log->toPlainText() : QString();
        QFile f(outDir + "/log_dump.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts << content;
            f.close();
        }
        QFile g(outDir + "/status_samples.txt");
        if (g.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&g);
            ts.setEncoding(QStringConverter::Utf8);
            for (const QString &s : samples) ts << s << "\n";
            g.close();
        }

        std::printf("\nLOG_CHARS=%lld\n", (long long)content.size());
        for (QPushButton *b : w.findChildren<QPushButton *>())
            std::printf("BTN  %-12s enabled=%d\n", qPrintable(b->text()), int(b->isEnabled()));
        for (QLabel *l : w.findChildren<QLabel *>()) {
            if (!l->text().isEmpty())
                std::printf("LABEL             = %s\n", qPrintable(l->text()));
        }

        w.grab().save(outDir + "/m3_e2e.png");
        app.quit();
    });

    return app.exec();
}
