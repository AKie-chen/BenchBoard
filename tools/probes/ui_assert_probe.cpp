// M1 验收探针：链接用户真实的 MainWindow.cpp，渲染并截图 + 硬断言。
// 不修改用户源码，只做观测。
#include "MainWindow.h"

#include <QApplication>
#include <QComboBox>
#include <QEventLoop>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTimer>
#include <cstdio>

static void pump(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString outDir = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");

    MainWindow w;
    w.resize(880, 520);
    w.show();
    pump(400);

    // ---------- 1. 清点控件 ----------
    const auto btns   = w.findChildren<QPushButton *>();
    const auto logs   = w.findChildren<QPlainTextEdit *>();
    const auto groups = w.findChildren<QGroupBox *>();
    const auto edits  = w.findChildren<QLineEdit *>();
    const auto spins  = w.findChildren<QSpinBox *>();
    const auto combos = w.findChildren<QComboBox *>();
    const auto labels = w.findChildren<QLabel *>();

    std::printf("== widget inventory ==\n");
    std::printf("QPushButton      = %d\n", int(btns.size()));
    std::printf("QPlainTextEdit   = %d\n", int(logs.size()));
    std::printf("QGroupBox        = %d\n", int(groups.size()));
    std::printf("QLineEdit        = %d\n", int(edits.size()));
    std::printf("QSpinBox         = %d\n", int(spins.size()));
    std::printf("QComboBox        = %d\n", int(combos.size()));
    if (!groups.isEmpty()) {
        std::printf("groupbox_title_utf8 = %s\n",
                    groups.first()->title().toUtf8().constData());
    }
    if (!combos.isEmpty()) {
        std::printf("combo_items = %d  first_utf8 = %s\n",
                    combos.first()->count(),
                    combos.first()->itemText(0).toUtf8().constData());
    }
    if (groups.isEmpty() || logs.isEmpty() || btns.isEmpty()) {
        std::printf("!! 关键控件缺失，布局不完整\n");
    }

    w.grab().save(outDir + QStringLiteral("/m1_a_initial.png"));

    // ---------- 2. 点按钮，验证信号槽真的通了 ----------
    const QString logBefore = logs.isEmpty() ? QString() : logs.first()->toPlainText();
    if (!btns.isEmpty()) {
        btns.first()->click();
        pump(300);
    }
    const QString logAfter = logs.isEmpty() ? QString() : logs.first()->toPlainText();
    std::printf("\n== signal/slot ==\n");
    std::printf("log_before_len = %d\n", int(logBefore.size()));
    std::printf("log_after_len  = %d\n", int(logAfter.size()));
    std::printf("log_delta_utf8 = [%s]\n",
                logAfter.mid(logBefore.size()).trimmed().toUtf8().constData());
    std::printf("VERDICT signal/slot = %s\n",
                (logAfter.size() > logBefore.size()) ? "PASS" : "FAIL");

    w.grab().save(outDir + QStringLiteral("/m1_b_clicked.png"));

    // ---------- 3. 拉高窗口，验证只有日志区跟着变高 ----------
    const int logH0   = logs.isEmpty()   ? 0 : logs.first()->height();
    const int grpH0   = groups.isEmpty() ? 0 : groups.first()->height();
    const int btnH0   = btns.isEmpty()   ? 0 : btns.first()->height();

    w.resize(880, 780);
    pump(400);

    const int logH1 = logs.isEmpty()   ? 0 : logs.first()->height();
    const int grpH1 = groups.isEmpty() ? 0 : groups.first()->height();
    const int btnH1 = btns.isEmpty()   ? 0 : btns.first()->height();

    std::printf("\n== resize 520 -> 780 (delta = 260) ==\n");
    std::printf("logView height   %4d -> %4d   (delta %+d)\n", logH0, logH1, logH1 - logH0);
    std::printf("configGroup      %4d -> %4d   (delta %+d)\n", grpH0, grpH1, grpH1 - grpH0);
    std::printf("startButton      %4d -> %4d   (delta %+d)\n", btnH0, btnH1, btnH1 - btnH0);
    const bool onlyLogGrows = (logH1 - logH0) > 200 && (grpH1 - grpH0) == 0 && (btnH1 - btnH0) == 0;
    std::printf("VERDICT stretch  = %s\n", onlyLogGrows ? "PASS" : "FAIL");

    w.grab().save(outDir + QStringLiteral("/m1_c_tall.png"));

    // ---------- 4. 状态栏文字 ----------
    std::printf("\n== statusbar ==\n");
    std::printf("currentMessage_utf8 = [%s]\n",
                w.statusBar()->currentMessage().toUtf8().constData());

    return 0;
}
