// ============================================================================
// m5_ui_probe —— M5 界面级验收（★ 不依赖 QProcess）
//
// 为什么需要这个探针：
//   本项目的执行上下文里 QProcess 起不了任何子进程（实测 QProcess: CreateFile
//   failed = ERROR_PIPE_BUSY，详见 docs/02 §3.22），所以 m3_e2e_probe 会直接
//   报 RESULT=ENV_NO_QPROCESS 退出。但界面里【除 k6 那一环之外】的东西
//   —— 三张图、汇总表格、陈旧文件陷阱 —— 全都可以脱离 k6 验证。
//
// 做法：链接【真实的】MainWindow.cpp，用 QMetaObject::invokeMethod 直接驱动
//       它的私有槽 onEngineFinished(exitCode, exitStatus)（槽在元对象里，
//       不需要改用户的源码把方法改成 public）。
//
// 检查项：
//   A 结构：表格存在 / 6 行 2 列 / 表头 / 真的是 central 的子控件
//   B 三图：3 个 QChartView、每图有 series、每条 series 绑了 2 条轴、
//           喂 36 个样本后【彩色像素 > 200】（沿用 m5_chart_paint_probe 判据）
//   C 正常退出：读 reports/summary.json → 表格数字与"直接读原始 JSON"跨源对账
//   D ★ 陈旧陷阱：先造一份"上一轮"的 summary.json，
//       ① 以 NormalExit 收尾 → 表格必须显示【上一轮】的数字（证明读取链路有效）
//       ② 再以 CrashExit 收尾（= 第二轮中途点停止）→ 表格必须【不】显示上一轮
//          的数字，且来源行必须写"实时统计"，请求数与状态栏跨源一致
//
// 运行：bash tools/probes/m5_ui_probe_run.sh
// ============================================================================

#include "MainWindow.h"
#include "RealtimeChartView.h"
#include "SummaryParser.h"
#include "TestOrchestrator.h"   // M7：界面唯一的后端入口，探针经它驱动终值
#include "types.h"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineSeries>
#include <QPainter>
#include <QPlainTextEdit>
#include <QStatusBar>
#include <QTableWidget>
#include <QValueAxis>

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok, const QString &detail = QString())
{
    std::printf("  [%s] %-48s %s\n", ok ? "PASS" : "FAIL", name,
                detail.toUtf8().constData());
    std::fflush(stdout);
    if (ok) ++g_pass; else ++g_fail;
}

// ---------------------------------------------------------------------------
// 彩色像素计数（与 m5_chart_paint_probe 同一判据）
//   轴 / 网格 / 文字都是灰阶(R≈G≈B) → 不计；QLineSeries 曲线是彩色 → 计入
// ---------------------------------------------------------------------------
static int countColorPixels(const QImage &src)
{
    const QImage img = src.convertToFormat(QImage::Format_ARGB32);
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = row[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            const int mx = qMax(r, qMax(g, b));
            const int mn = qMin(r, qMin(g, b));
            if (mx - mn > 30) ++n;
        }
    }
    return n;
}

static QImage renderWidget(QWidget *w)
{
    QImage img(w->size(), QImage::Format_ARGB32);
    img.fill(Qt::white);
    QPainter p(&img);
    w->render(&p);
    p.end();
    return img;
}

static const char *kSummaryPath = BENCHBOARD_ROOT "/reports/summary.json";

// ---------------------------------------------------------------------------
// 造一份"上一轮"留下的 summary.json —— 数字刻意扎眼，便于和实时路径区分
// ---------------------------------------------------------------------------
static bool writeFakeSummary(qint64 count, double failedRate,
                             double avg, double p95, double p99)
{
    QJsonObject dur;
    dur.insert(QStringLiteral("avg"),   avg);
    dur.insert(QStringLiteral("min"),   avg * 0.5);
    dur.insert(QStringLiteral("med"),   avg);
    dur.insert(QStringLiteral("max"),   p99 * 2.0);
    dur.insert(QStringLiteral("p(90)"), p95 * 0.9);
    dur.insert(QStringLiteral("p(95)"), p95);
    dur.insert(QStringLiteral("p(99)"), p99);
    // ★ 真实产出里 http_req_duration 内会混进一个 thresholds 子对象 —— 一起造上，
    //   这样这份样本同时也是"带非数值键"的回归用例
    QJsonObject th; th.insert(QStringLiteral("p(95)<500"), false);
    dur.insert(QStringLiteral("thresholds"), th);

    QJsonObject reqs;
    reqs.insert(QStringLiteral("count"), double(count));
    reqs.insert(QStringLiteral("rate"),  double(count) / 10.0);

    QJsonObject failed;
    failed.insert(QStringLiteral("value"), failedRate);

    QJsonObject metrics;
    metrics.insert(QStringLiteral("http_req_duration"), dur);
    metrics.insert(QStringLiteral("http_reqs"),         reqs);
    metrics.insert(QStringLiteral("http_req_failed"),   failed);

    QJsonObject root;
    root.insert(QStringLiteral("name"), QString());
    root.insert(QStringLiteral("path"), QString());

    QJsonObject doc;
    doc.insert(QStringLiteral("root_group"), root);
    doc.insert(QStringLiteral("metrics"),    metrics);

    QFile f(QString::fromLatin1(kSummaryPath));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(doc).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

// 独立读原始 JSON（不走 SummaryParser）—— 用于跨源对账
struct RawSummary {
    bool   ok = false;
    qint64 count = 0;
    double failedRate = 0.0;
    double avg = 0.0, p95 = 0.0, p99 = 0.0;
};

static RawSummary readRawSummary()
{
    RawSummary r;
    QFile f(QString::fromLatin1(kSummaryPath));
    if (!f.open(QIODevice::ReadOnly)) return r;
    const QJsonObject doc    = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonObject metrics = doc.value(QStringLiteral("metrics")).toObject();
    const QJsonObject dur    = metrics.value(QStringLiteral("http_req_duration")).toObject();
    r.count      = qint64(metrics.value(QStringLiteral("http_reqs")).toObject()
                              .value(QStringLiteral("count")).toDouble());
    r.failedRate = metrics.value(QStringLiteral("http_req_failed")).toObject()
                       .value(QStringLiteral("value")).toDouble();
    r.avg = dur.value(QStringLiteral("avg")).toDouble();
    r.p95 = dur.value(QStringLiteral("p(95)")).toDouble();
    r.p99 = dur.value(QStringLiteral("p(99)")).toDouble();
    r.ok = true;
    return r;
}

static QString cell(QTableWidget *t, int row, int col)
{
    QTableWidgetItem *it = t->item(row, col);
    return it ? it->text() : QStringLiteral("<null>");
}

// ★ M7 之后终值归编排层：MainWindow 里已经没有 onEngineFinished 这个槽了，
//   所以驱动的是 TestOrchestrator 的同名槽（私有槽在元对象里，invokeMethod 照样能调）。
static void invokeFinished(MainWindow *mw, int exitCode, bool cleanExit)
{
    TestOrchestrator *orch = mw->findChild<TestOrchestrator *>();
    if (!orch) {
        std::printf("        (找不到 TestOrchestrator —— 界面结构变了？)\n");
        return;
    }
    const bool ok = QMetaObject::invokeMethod(
        orch, "onEngineFinished", Qt::DirectConnection,
        Q_ARG(int, exitCode), Q_ARG(bool, cleanExit),
        Q_ARG(QString, QString::fromLatin1(kSummaryPath)));
    std::printf("        (invokeMethod onEngineFinished 返回 %s)\n", ok ? "true" : "false");
    QApplication::processEvents();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

    // =======================================================================
    std::printf("\n========== Part A：汇总表格结构 ==========\n");
    // =======================================================================
    MainWindow mw;
    mw.resize(1100, 900);
    mw.show();
    QApplication::processEvents();

    QTableWidget *table = mw.findChild<QTableWidget *>();
    check("A1 表格控件存在（findChild<QTableWidget*>）", table != nullptr);
    if (!table) { std::printf("RESULT=ABORT 表格都找不到，后续无法进行\n"); return 2; }

    check("A2 6 行 × 2 列",
          table->rowCount() == 6 && table->columnCount() == 2,
          QStringLiteral("rowCount=%1 colCount=%2").arg(table->rowCount()).arg(table->columnCount()));

    const QString h0 = table->horizontalHeaderItem(0) ? table->horizontalHeaderItem(0)->text() : QString();
    const QString h1 = table->horizontalHeaderItem(1) ? table->horizontalHeaderItem(1)->text() : QString();
    check("A3 表头 = {指标, 数值}",
          h0 == QStringLiteral("指标") && h1 == QStringLiteral("数值"),
          QStringLiteral("{%1, %2}").arg(h0, h1));

    check("A4 表格尺寸非空（真的进了布局）",
          table->width() > 100 && table->height() > 50,
          QStringLiteral("%1 x %2").arg(table->width()).arg(table->height()));

    // =======================================================================
    std::printf("\n========== Part B：三张图（像素判据） ==========\n");
    // =======================================================================
    RealtimeChartView *rv = mw.findChild<RealtimeChartView *>();
    check("B1 RealtimeChartView 挂在 MainWindow 上", rv != nullptr);
    if (rv) {
        // 喂 36 个样本 —— 和 m5_chart_paint_probe 一样喂满横轴，
        // 否则曲线只占 1/3 幅面，像素数会被误判成 FAIL
        const qint64 base = 1758700000000LL;
        for (int i = 1; i <= 36; ++i) {
            WindowSample w;
            w.epochMs       = base + qint64(i) * 1000;
            w.requestCount  = 1150 + i * 12;
            w.rps           = double(w.requestCount);
            w.vus           = 8;
            w.hasVusSample  = true;
            w.avgDurationMs = 3.2 + i * 0.1;
            w.p95DurationMs = 7.4 + i * 0.25;
            w.errorRate     = 0.0;
            rv->appendSample(w);
        }
        QApplication::processEvents();

        const QList<QChartView *> views = mw.findChildren<QChartView *>();
        check("B2 找到 3 个 QChartView",
              views.size() == 3,
              QStringLiteral("实际 %1").arg(views.size()));

        std::printf("        %-18s %6s %6s %8s %10s %6s\n",
                    "chart 标题", "series", "点数", "绑定轴", "彩色像素", "判定");
        int idx = 0;
        for (QChartView *v : views) {
            QChart *ch = v->chart();
            int pts = 0, maxAtt = -1, nSeries = ch ? int(ch->series().size()) : -1;
            if (ch) {
                for (QAbstractSeries *s : ch->series()) {
                    if (auto *ls = qobject_cast<QLineSeries *>(s)) {
                        pts += int(ls->count());
                        maxAtt = qMax(maxAtt, int(ls->attachedAxes().size()));
                    }
                }
            }
            v->resize(480, 320);
            const int px = countColorPixels(renderWidget(v));
            const bool ok = (pts > 0) && (maxAtt >= 2) && (px > 200);
            std::printf("        %-18s %6d %6d %8d %10d %6s\n",
                        ch ? ch->title().toUtf8().constData() : "(null)",
                        nSeries, pts, maxAtt, px, ok ? "OK" : "FAIL");
            check(QStringLiteral("B%1 %2 画出了曲线").arg(idx + 3)
                      .arg(ch ? ch->title() : QStringLiteral("?")).toUtf8().constData(),
                  ok, QStringLiteral("彩色像素 = %1 (>200)").arg(px));
            renderWidget(v).save(QStringLiteral("ui_probe_view%1.png").arg(++idx));
        }
    }

    // =======================================================================
    std::printf("\n========== Part C：正常退出 → 读 summary.json（★ 三源对账） ==========\n");
    // 三个互不相干的来源必须算出同一个数：
    //   ① 表格（MainWindow 走 SummaryParser + 格式化）
    //   ② 探针自己用 QJsonDocument 直接读原始 JSON
    //   ③ 语料的【文档记录期望值】（写死在下面，是人核对过的常量）
    // 用固定语料而不是 reports/ 里碰巧存在的文件 —— 判据必须是确定的。
    // =======================================================================
    const QString corpus = QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_summary_sample.json");
    QFile::remove(QString::fromLatin1(kSummaryPath));
    check("C0 装入语料样本作为 summary.json", QFile::copy(corpus, QString::fromLatin1(kSummaryPath)),
          corpus);

    const RawSummary raw = readRawSummary();
    check("C0b 探针可独立解析原始 JSON", raw.ok);

    // --- 来源 ② vs 来源 ③：先证明语料没被动过 ---
    const qint64 kExpectCount  = 4375;
    const double kExpectAvg    = 3.0926225599999984;
    const double kExpectP95    = 4.0206;
    const double kExpectP99    = 4.828576000000002;
    check("C0c 语料 count == 文档记录值 4375", raw.count == kExpectCount,
          QStringLiteral("JSON=%1").arg(raw.count));
    check("C0d 语料 avg == 文档记录值 3.09262256",
          qAbs(raw.avg - kExpectAvg) < 1e-9, QStringLiteral("JSON=%1").arg(raw.avg, 0, 'g', 12));

    if (raw.ok) {
        invokeFinished(&mw, 0, true);

        // --- 来源 ① vs 来源 ③：表格是否等于【文档记录值】 ---
        check("C1 请求总数 == 4375",
              cell(table, 0, 1) == QStringLiteral("4375"),
              QStringLiteral("表格=%1").arg(cell(table, 0, 1)));
        check("C2 失败数 == 0（语料 http_req_failed.value = 0）",
              cell(table, 1, 1) == QStringLiteral("0"),
              QStringLiteral("表格=%1").arg(cell(table, 1, 1)));
        check("C3 平均值 == \"3.09 ms\"", cell(table, 2, 1) == QStringLiteral("3.09 ms"),
              QStringLiteral("表格=%1").arg(cell(table, 2, 1)));
        check("C4 p95 == \"4.02 ms\"", cell(table, 3, 1) == QStringLiteral("4.02 ms"),
              QStringLiteral("表格=%1").arg(cell(table, 3, 1)));
        check("C5 p99 == \"4.83 ms\"", cell(table, 4, 1) == QStringLiteral("4.83 ms"),
              QStringLiteral("表格=%1").arg(cell(table, 4, 1)));
        check("C6 来源行 = k6 汇总（summary.json）",
              cell(table, 5, 1) == QStringLiteral("k6 汇总（summary.json）"),
              QStringLiteral("实际=%1").arg(cell(table, 5, 1)));

        // --- 来源 ① vs 来源 ②：表格与原始 JSON 逐格对账 ---
        check("C7 表格请求总数 == 探针直读 JSON",
              cell(table, 0, 1) == QString::number(raw.count),
              QStringLiteral("表格=%1 直读=%2").arg(cell(table, 0, 1)).arg(raw.count));
        check("C8 表格 p95 == 探针直读 JSON p(95)",
              cell(table, 3, 1) == QStringLiteral("%1 ms").arg(raw.p95, 0, 'f', 2),
              QStringLiteral("表格=%1 直读=%2 ms").arg(cell(table, 3, 1)).arg(raw.p95, 0, 'f', 2));
    }

    // =======================================================================
    std::printf("\n========== Part D：★ 陈旧文件陷阱（连跑两轮，第二轮中途停） ==========\n");
    // =======================================================================
    const qint64 kStaleCount = 424242;
    const double kStaleRate  = 0.5;
    check("D0 造一份\"上一轮\"的 summary.json (count=424242, 失败率=0.5)",
          writeFakeSummary(kStaleCount, kStaleRate, 11.11, 22.22, 33.33));

    // 第 1 轮：正常跑完 → 表格应当读到这份文件
    invokeFinished(&mw, 0, true);
    check("D1 第 1 轮（NormalExit）表格确实读了文件（=424242）",
          cell(table, 0, 1) == QStringLiteral("424242"),
          QStringLiteral("实际=%1").arg(cell(table, 0, 1)));
    check("D2 第 1 轮失败数 = round(0.5 × 424242) = 212121",
          cell(table, 1, 1) == QStringLiteral("212121"),
          QStringLiteral("实际=%1").arg(cell(table, 1, 1)));

    // 第 2 轮：中途点停止（k6 被强杀 → CrashExit）→ 表格绝不能沿用上一轮文件
    invokeFinished(&mw, 62097, false);
    {
        const QString req = cell(table, 0, 1);
        const QString src = cell(table, 5, 1);
        check("D3 ★ 第 2 轮（CrashExit）表格【不】沿用上一轮文件",
              req != QStringLiteral("424242") && cell(table, 1, 1) != QStringLiteral("212121"),
              QStringLiteral("请求总数=%1（不得为 424242）").arg(req));
        check("D4 来源行 = 实时统计（未经 k6 汇总）",
              src.contains(QStringLiteral("实时统计")),
              QStringLiteral("实际=%1").arg(src));
        check("D5 耗时三行如实显示 \"—\"（实时侧没有分布）",
              cell(table, 2, 1) == QStringLiteral("—")
                  && cell(table, 3, 1) == QStringLiteral("—")
                  && cell(table, 4, 1) == QStringLiteral("—"),
              QStringLiteral("avg/p95/p99 = %1 / %2 / %3")
                  .arg(cell(table, 2, 1), cell(table, 3, 1), cell(table, 4, 1)));

        // 跨源对账：表格的"请求总数"必须等于状态栏那句话里的"请求 N 次"
        // （两条路都读 m_agg.stats()，但一条经 setItem、一条经 showMessage）
        const QString msg = mw.statusBar()->currentMessage();
        qint64 barReq = -1;
        const int i = msg.indexOf(QStringLiteral("请求 "));
        if (i >= 0) {
            int j = i + QStringLiteral("请求 ").size();
            QString num;
            while (j < msg.size() && msg.at(j).isDigit()) num.append(msg.at(j++));
            if (!num.isEmpty()) barReq = num.toLongLong();
        }
        check("D6 跨源对账：表格请求数 == 状态栏请求数",
              barReq >= 0 && QString::number(barReq) == req,
              QStringLiteral("表格=%1  状态栏=%2").arg(req).arg(barReq));
    }

    // =======================================================================
    std::printf("\n==================== 汇总 ====================\n");
    std::printf("PASS = %d   FAIL = %d\n", g_pass, g_fail);
    std::printf("RESULT=%s\n", g_fail == 0 ? "ALL_PASS" : "HAS_FAIL");
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
