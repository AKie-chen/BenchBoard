// ============================================================================
// M4 骨架下发前自测 —— 把 RealtimeChartView.cpp / MetricsAggregator.cpp
// 的 TODO 提示原样填一遍，验证「照着 TODO 写真的能编译、能跑、行为对」。
//
// 它【不碰 src/】，只是把 TODO 里的示例代码复制到这个独立工程里编译。
// 目的有两个：
//   ① 防止我把写错的 API 塞进 TODO 里，让用户对着一条编译不过的提示浪费时间
//   ② 把「QChart 必须堆分配」这条从"据说"变成"实测"（A/B 对照）
//
// 跑法：bash tools/probes/m4_skeleton_check_run.sh
// ============================================================================

#include "RealtimeChartView.h"
#include "MetricsAggregator.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QApplication>
#include <QDateTime>
#include <QPainter>
#include <QVBoxLayout>
#include <cstdio>

// ============================================================================
// 第一部分：RealtimeChartView 的「答案版」实现
// 下面这段就是 RealtimeChartView.cpp 里 TODO(M4-3) ~ TODO(M4-6) 的示例代码原样。
// ============================================================================

RealtimeChartView::RealtimeChartView(QWidget *parent)
    : QWidget(parent)
{
    // TODO(M4-3)
    m_chart = new QChart();
    m_chart->setTitle(QStringLiteral("请求速率 RPS"));
    m_chart->legend()->hide();

    m_view = new QChartView(m_chart, this);
    m_view->setRenderHint(QPainter::Antialiasing);

    m_rpsSeries = new QLineSeries();
    m_chart->addSeries(m_rpsSeries);

    m_axisX = new QValueAxis();
    m_axisX->setTitleText(QStringLiteral("秒"));
    m_axisX->setRange(0, 30);

    m_axisY = new QValueAxis();
    m_axisY->setTitleText(QStringLiteral("req/s"));
    m_axisY->setRange(0, 100);

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_rpsSeries->attachAxis(m_axisX);
    m_rpsSeries->attachAxis(m_axisY);

    QVBoxLayout *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_view);
}

void RealtimeChartView::setMaxPoints(int n)
{
    m_maxPoints = qMax(1, n);
}

int RealtimeChartView::maxPoints() const
{
    return m_maxPoints;
}

void RealtimeChartView::reset()
{
    // TODO(M4-6)
    m_points.clear();
    m_rpsSeries->replace(m_points);
    m_t0Ms = 0;
    m_axisX->setRange(0, 30);
    m_axisY->setRange(0, 100);
    m_view->update();
}

void RealtimeChartView::appendSample(const WindowSample &sample)
{
    // TODO(M4-4)
    if (m_t0Ms == 0) m_t0Ms = sample.epochMs;
    const double x = double(sample.epochMs - m_t0Ms) / 1000.0;
    const double y = sample.rps;

    m_points.append(QPointF(x, y));
    while (m_points.size() > m_maxPoints) m_points.removeFirst();

    // TODO(M4-5)
    m_rpsSeries->replace(m_points);

    double maxY = 0.0;
    for (const QPointF &p : m_points) maxY = qMax(maxY, p.y());
    m_axisY->setRange(0.0, qMax(10.0, maxY * 1.15));

    m_axisX->setRange(0.0, qMax(30.0, x + 1.0));
}

// ============================================================================
// 第二部分：MetricsAggregator.cpp TODO(M4-7) 的示例代码原样（换了个函数名，
// 因为真正的 takeWindow 已经由 src/MetricsAggregator.cpp 提供，不能重复定义）
// ============================================================================

static WindowSample demoTakeWindow(const MetricsAggregator::Snapshot &s,
                                   qint64 &lastReq, qint64 &lastMs)
{
    WindowSample w;
    w.epochMs      = QDateTime::currentMSecsSinceEpoch();
    w.requestCount = s.requests - lastReq;
    const qint64 dMs = s.elapsedMs - lastMs;
    w.rps = double(w.requestCount) / qMax(1e-3, double(dMs) / 1000.0);
    w.vus          = s.vus;
    w.hasVusSample = (s.vus > 0);
    lastReq = s.requests;
    lastMs  = s.elapsedMs;
    return w;
}

// ============================================================================
// 第三部分：A/B 对照 —— 栈分配 vs 堆分配
// ============================================================================

// 错误写法：chart / series / axis 全是栈对象。
// QChartView 的 scene 会 addItem(chart)，chart->addSeries 又接管 series；
// 函数一返回，这些栈对象全部析构，而 scene 手里还是它们的地址。
static int stackVersion()
{
    std::printf("  [栈版] ① 栈对象已建好（chart / series / axisX / axisY）\n");
    QChart      chart;
    QLineSeries series;
    QValueAxis  axisX;
    QValueAxis  axisY;

    chart.addSeries(&series);
    chart.addAxis(&axisX, Qt::AlignBottom);
    chart.addAxis(&axisY, Qt::AlignLeft);
    series.attachAxis(&axisX);
    series.attachAxis(&axisY);
    std::printf("  [栈版] ② 已装配：addSeries / addAxis / attachAxis\n");

    QChartView view(&chart);
    view.resize(300, 200);
    view.show();                       // 触发一次真实的 scene 装配
    std::printf("  [栈版] ③ view.show() 返回了\n");

    QApplication::processEvents();
    std::printf("  [栈版] ④ processEvents 也过了，函数即将返回，栈对象将被析构…\n");
    return 0;                          // ← 返回瞬间 chart/series/axis 全部析构
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // ★ 关键：崩溃时也要看到崩溃前的输出。
                                           //   stdout 接管道时是块缓冲，进程被异常终止
                                           //   会丢掉整个缓冲区 —— 现象就是"程序什么
                                           //   都没打印就没了"，很容易误判成"没跑起来"
    QApplication app(argc, argv);

    const QString mode = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("heap");
    std::printf("模式: %s\n\n", qPrintable(mode));

    if (mode == QStringLiteral("stack")) {
        stackVersion();
        std::printf("  [栈版] ⑤ 已安全返回 —— 没有崩\n");
        // 走到这里说明【没有崩】—— 那就再让事件循环转一圈，
        // 逼 scene 去访问已经析构的对象
        QApplication::processEvents();
        std::printf("  [栈版] ⑥ 退出后仍存活 —— 本次未复现崩溃\n");
        std::printf("RESULT=STACK_SURVIVED\n");
        return 0;
    }

    // ---------- 堆版：应该是完全正常的 ----------
    int fail = 0;
    RealtimeChartView rv;
    rv.resize(640, 360);
    rv.setMaxPoints(100);

    // 通过 findChild 拿到 view → chart → series，这是【从外部】检查
    // private 成员的唯一办法（也给用户示范怎么在探针里做黑盒断言）
    QChartView *view = rv.findChild<QChartView *>();
    if (!view || !view->chart()) {
        std::printf("FAIL 找不到 QChartView / QChart —— 视图没挂上\n");
        return 1;
    }
    QChart *chart = view->chart();
    const QList<QAbstractSeries *> all = chart->series();
    if (all.isEmpty()) {
        std::printf("FAIL chart 里没有 series —— addSeries 漏了\n");
        return 1;
    }
    QLineSeries *series = qobject_cast<QLineSeries *>(all.first());
    if (!series) {
        std::printf("FAIL series 类型不对\n");
        return 1;
    }

    // 断言 ① 轴绑上了（attachAxis 漏了的典型症状：曲线不显示但零报错）
    const QList<QAbstractAxis *> axX = series->attachedAxes();
    std::printf("  series->attachedAxes().size() = %lld  (期望 2)\n", (long long)axX.size());
    if (axX.size() != 2) { std::printf("FAIL attachAxis 漏了\n"); ++fail; }

    // 断言 ② 喂 300 个点，maxPoints=100 → 应该只剩 100 个
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < 300; ++i) {
        WindowSample w;
        w.epochMs      = t0 + qint64(i) * 1000;
        w.requestCount = 100 + i;
        w.rps          = 100.0 + i;
        w.vus          = 8;
        w.hasVusSample = true;
        rv.appendSample(w);
    }
    std::printf("  喂 300 点后 series->count() = %lld  (期望 100，限长生效)\n",
                (long long)series->count());
    if (series->count() != 100) { std::printf("FAIL 限长没生效\n"); ++fail; }

    // 断言 ③ 最后一个点的 Y == 399（rps 从 100 递增到 399），X 轴右边界 >= 300
    const QPointF last = series->at(series->count() - 1);
    const qreal xMax = chart->axes(Qt::Horizontal).isEmpty()
                           ? 0.0
                           : qobject_cast<QValueAxis *>(chart->axes(Qt::Horizontal).first())->max();
    std::printf("  最后一点 = (%.1f, %.1f)   轴范围 0 ~ %.1f\n", last.x(), last.y(), xMax);
    if (qAbs(last.y() - 399.0) > 0.001 || last.x() < 299.0) {
        std::printf("FAIL 点坐标不对\n"); ++fail;
    }
    if (xMax < 300.0) { std::printf("FAIL X 轴范围没跟着走\n"); ++fail; }

    // 断言 ④ reset() 之后清空
    rv.reset();
    std::printf("  reset() 后 series->count() = %lld  (期望 0)\n", (long long)series->count());
    if (series->count() != 0) { std::printf("FAIL reset 没清干净\n"); ++fail; }

    // 断言 ⑤ reset 之后第一个点的 X 重新从 0 开始（m_t0Ms 归零了）
    {
        WindowSample w;
        w.epochMs = t0 + 999999;       // 故意给一个很晚的时间
        w.rps     = 42.0;
        rv.appendSample(w);
        const QPointF p = series->at(0);
        std::printf("  reset 后首点 = (%.1f, %.1f)  (期望 x=0, y=42)\n", p.x(), p.y());
        if (qAbs(p.x()) > 0.001 || qAbs(p.y() - 42.0) > 0.001) {
            std::printf("FAIL m_t0Ms 没归零\n"); ++fail;
        }
    }

    // 断言 ⑥ takeWindow 片段：0 请求时 rps 必须是 0，不能是 inf / nan
    {
        MetricsAggregator::Snapshot s;
        qint64 lastReq = 0, lastMs = 0;
        const WindowSample w = demoTakeWindow(s, lastReq, lastMs);
        std::printf("  空窗口: requestCount=%lld rps=%g  (期望 0 / 0，不能是 inf/nan)\n",
                    (long long)w.requestCount, w.rps);
        if (w.requestCount != 0 || !qIsFinite(w.rps) || w.rps != 0.0) {
            std::printf("FAIL 空窗口 rps 算错了\n"); ++fail;
        }
        // 再来一次：累计值从 0 涨到 500，elapsedMs 涨到 1000 → rps = 500
        s.requests = 500; s.elapsedMs = 1000;
        const WindowSample w2 = demoTakeWindow(s, lastReq, lastMs);
        std::printf("  满窗口: requestCount=%lld rps=%.1f  (期望 500 / 500)\n",
                    (long long)w2.requestCount, w2.rps);
        if (w2.requestCount != 500 || qAbs(w2.rps - 500.0) > 0.01) {
            std::printf("FAIL 增量换算错了\n"); ++fail;
        }
    }

    std::printf("\nRESULT=%s\n", fail == 0 ? "ALL_PASS" : "HAS_FAIL");
    return fail == 0 ? 0 : 1;
}
