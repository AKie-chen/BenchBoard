// ============================================================================
// m4_window_probe —— M4 的守恒断言（链接真实的 src/ 实现，不模拟）
//
// 【为什么必须有它】
//   M4 的典型 bug 是「算完不返回」：takeWindow() 里四步全算对了，
//   最后却写成 `return WindowSample{};` —— 返回一个默认构造的空对象。
//   症状极具欺骗性：图表照显示、标题照有、坐标轴照画、状态栏数字照跳，
//   **只有曲线一条线都没有**（rps/epochMs 全 0 → 所有点重合在原点）。
//   编译 0 警告、运行 0 报错、/W4 也不报（w 被赋值过，C4189 不触发）。
//   人看不到屏幕的时候，只有守恒断言能判定。
//
// 【守恒式】Σ takeWindow().requestCount  ==  喂进去的 http_reqs 总增量
//   少了 = 丢数据；多了 = 重复计；全新构造的空对象 = 全 0，直接对不上。
//
// 【断言清单】
//   A1  ΣrequestCount == 142                ← 抓「算完不返回」/「水位不推进」
//   A2  至少一个 epochMs != 0               ← 抓「返回默认构造的空对象」
//   A3  至少一个 rps > 0
//   A4  第二次 takeWindow 是空窗口           ← 『取走』语义
//   A5  能拿到 QLineSeries                   ← chart 装配 + attachAxis 到位
//   A6  曲线点数 == 喂进去的 sample 数        ← 抓「喂了但没画」
//   A7  曲线 y 值不全为 0                    ← ★「有图没线」的直接判据
//   A8  reset() 后重跑 Σ 仍是 142（非 284）   ← 抓「reset 没清水位」
// ============================================================================

#include "MetricsAggregator.h"
#include "RealtimeChartView.h"

#include <QApplication>
#include <QFile>
#include <QList>
#include <QPointF>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>

#include <cstdio>
#include <cstdlib>

static int g_pass = 0;
static int g_fail = 0;

static void check(bool ok, const char *name, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("  [PASS] %s\n", name);
    } else {
        ++g_fail;
        if (detail.isEmpty())
            std::printf("  [FAIL] %s\n", name);
        else
            std::printf("  [FAIL] %s  (%s)\n", name, qPrintable(detail));
    }
}

static QByteArray loadCorpus()
{
    QFile f(QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_stream_sample_2k.jsonl"));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("!! 无法打开语料: %s\n", qPrintable(f.fileName()));
        std::exit(2);
    }
    return f.readAll();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // 无缓冲：异常终止时也要能看到已打印的内容
    QApplication app(argc, argv);

    const QByteArray corpus = loadCorpus();
    std::printf("语料: %lld 字节（期望 requests = 142）\n\n", (long long)corpus.size());

    // ==================== 第一轮：整块喂 ====================
    std::printf("--- 第一轮：整块喂入全部语料 ---\n");
    MetricsAggregator agg;
    RealtimeChartView chart;

    agg.reset();
    agg.start();
    agg.feed(corpus);

    QVector<WindowSample> samples;
    for (int i = 0; i < 5; ++i) {
        const WindowSample w = agg.takeWindow();
        samples.append(w);
        chart.appendSample(w);            // 走真实路径：窗口 → 曲线
    }

    qint64 sumRequests = 0;
    bool   anyEpoch    = false;
    bool   anyRps      = false;
    for (const WindowSample &w : samples) {
        sumRequests += w.requestCount;
        if (w.epochMs != 0) anyEpoch = true;
        if (w.rps > 0.0)    anyRps   = true;
    }

    check(sumRequests == 142,
          "A1  [守恒] ΣrequestCount == 142  （少了=丢数据，多了=重复计）",
          QStringLiteral("实得 %1").arg(sumRequests));
    check(anyEpoch,
          "A2  至少一个 sample.epochMs != 0  （曲线 X 轴的基准时间）",
          QStringLiteral("5 个 sample 全是 0 → 所有点重合在原点"));
    check(anyRps,
          "A3  至少一个 sample.rps > 0",
          QStringLiteral("rps 全为 0 → 画出来是一条压在 X 轴上的线"));
    check(samples.size() >= 2 && samples[1].requestCount == 0,
          "A4  第二次 takeWindow 是空窗口  （『取走』语义：水位已推到当前位置）");

    // ==================== 曲线侧：序列里到底有没有点 ====================
    std::printf("\n--- 曲线侧：QLineSeries 里到底有没有点 ---\n");
    QChartView *cv = chart.findChild<QChartView *>();
    QLineSeries *ls = nullptr;
    if (cv != nullptr && cv->chart() != nullptr && !cv->chart()->series().isEmpty())
        ls = qobject_cast<QLineSeries *>(cv->chart()->series().first());

    check(ls != nullptr, "A5  能从控件里找到 QLineSeries（chart 装配 + attachAxis 到位）");
    if (ls != nullptr) {
        check(ls->count() == samples.size(),
              "A6  曲线点数 == 喂进去的 sample 数",
              QStringLiteral("series 有 %1 点，喂了 %2 个")
                  .arg(ls->count()).arg(samples.size()));

        bool anyNonZeroY = false;
        const QList<QPointF> pts = ls->points();
        for (const QPointF &p : pts) {
            if (p.y() != 0.0) { anyNonZeroY = true; break; }
        }
        check(anyNonZeroY,
              "A7  曲线 y 值不全为 0  ★『有图没线』的直接判据",
              QStringLiteral("所有点 y 都是 0 → 画再多点也只是一条贴着 X 轴的线"));
    }

    // ==================== 第二轮：reset 有没有清水位 ====================
    std::printf("\n--- 第二轮：reset() 后重跑 ---\n");
    agg.reset();
    agg.start();
    agg.feed(corpus);

    qint64 sum2 = 0;
    for (int i = 0; i < 3; ++i) sum2 += agg.takeWindow().requestCount;

    check(sum2 == 142,
          "A8  reset() 后重跑 Σ 仍是 142 而非 284  （水位清干净了）",
          QStringLiteral("实得 %1").arg(sum2));

    std::printf("\n================ 结果 ================\n");
    std::printf("PASS %d / FAIL %d\n", g_pass, g_fail);
    std::printf("RESULT=%s\n", g_fail == 0 ? "ALL_PASS" : "HAS_FAILURE");
    return g_fail == 0 ? 0 : 1;
}
