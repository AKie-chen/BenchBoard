#include "RealtimeChartView.h"

#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLegend>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QColor>
#include <QGridLayout>
#include <QPainter>

// ============================================================================
// 三张图（RPS / VU / 延迟）+ 一个 ChartContext
//
// 装配顺序（★ 顺序错了不会报错，只是画不出线）：
//     ① new QChart + setTitle
//     ② new QChartView(chart, host) —— 这一步之后 chart 归 scene 所有，
//        所以这条链上的对象【全部必须 new】（实测：栈版 exit=139）
//     ③ chart->addSeries(series)              ← 必须在 attachAxis 【之前】
//     ④ chart->addAxis(axisX / axisY, ...)
//     ⑤ series->attachAxis(axisX) / attachAxis(axisY)
//
// ★ attachAxis 写在 addSeries 之前 → Qt 直接返回 false，只在
//   OutputDebugString 里留一句 qWarning（Windows 上不挂调试器看不见）。
//   症状：有标题、有坐标轴、有网格 —— 一条线都没有，且零报错。
//
// ★ 还有一个症状完全相同却不报错的原因：Y 轴范围不含数据 → 曲线画到画布外。
//   所以诊断顺序是【先看 attachedAxes().size()，再看 Y 轴范围】。
//   20 秒自查：bash tools/probes/m5_chart_paint_probe_run.sh
//   判据是渲染成图后的【彩色像素数】（轴和文字是灰阶、曲线是彩色），
//   目标形态：每张图「绑定轴数 ≥ 2、彩色像素 > 200」。
// ============================================================================

namespace {

// --- 配色：谁的线谁上色 ---
// ★ 颜色属于「每条线一份」，归 series 的创建方；不属于 ChartContext
//   （ChartContext 只装每张图一份的 chart / view / 两个轴）。
const QColor kColorRps(0x2F, 0x81, 0xF7);   // 蓝
const QColor kColorVu (0x22, 0xA1, 0x6B);   // 绿
const QColor kColorAvg(0x2F, 0x81, 0xF7);   // 蓝 —— 平均值
const QColor kColorP95(0xF0, 0x8C, 0x00);   // 橙 —— p95

// 取一条曲线里最大的 y。延迟图两条线共轴，范围必须【一起算】。
double maxYOf(const QVector<QPointF> &points)
{
    double m = 0.0;
    for (const QPointF &p : points) m = qMax(m, p.y());
    return m;
}

}  // namespace

RealtimeChartView::RealtimeChartView(QWidget *parent)
    : QWidget(parent)
{
    // --- RPS 图：一条线 ---
    m_rpsctx = makeChart(QStringLiteral("请求速率 RPS"), QStringLiteral("req/s"), this);
    m_rpsSeries = new QLineSeries();
    m_rpsSeries->setColor(kColorRps);
    m_rpsctx.m_chart->addSeries(m_rpsSeries);        // ① 先进 chart
    m_rpsSeries->attachAxis(m_rpsctx.m_axisX);       // ② 再绑轴 ← 顺序不能反
    m_rpsSeries->attachAxis(m_rpsctx.m_axisY);

    // --- VU 图：一条线 ---
    m_vuctx = makeChart(QStringLiteral("虚拟用户数 VU"), QStringLiteral("count"), this);
    m_vuSeries = new QLineSeries();
    m_vuSeries->setColor(kColorVu);
    m_vuctx.m_chart->addSeries(m_vuSeries);
    m_vuSeries->attachAxis(m_vuctx.m_axisX);
    m_vuSeries->attachAxis(m_vuctx.m_axisY);

    // --- 延迟图：两条线共用一套轴 ---
    // ★ 必须打开图例：avg 和 p95 两条线，没有图例就没人分得清哪条是哪个。
    //   （makeChart 里默认 hide()；另外两张图只有一条线，不需要图例。）
    m_latctx = makeChart(QStringLiteral("响应时间"), QStringLiteral("ms"), this);
    m_latctx.m_chart->legend()->show();
    m_latctx.m_chart->legend()->setAlignment(Qt::AlignBottom);

    m_avgSeries = new QLineSeries();
    m_avgSeries->setName(QStringLiteral("平均值"));
    m_avgSeries->setColor(kColorAvg);
    m_latctx.m_chart->addSeries(m_avgSeries);
    m_avgSeries->attachAxis(m_latctx.m_axisX);
    m_avgSeries->attachAxis(m_latctx.m_axisY);

    m_p95Series = new QLineSeries();
    m_p95Series->setName(QStringLiteral("p95"));
    m_p95Series->setColor(kColorP95);
    m_latctx.m_chart->addSeries(m_p95Series);
    m_p95Series->attachAxis(m_latctx.m_axisX);
    m_p95Series->attachAxis(m_latctx.m_axisY);

    // --- 布局：2×2，延迟图横跨整行（它两条线，给宽一点）---
    // ★ 三个 view 都要 addWidget —— 少一个就是"只显示最后一个"，零报错
    QGridLayout *lay = new QGridLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_rpsctx.m_view, 0, 0);
    lay->addWidget(m_vuctx.m_view,  0, 1);
    lay->addWidget(m_latctx.m_view, 1, 0, 1, 2);
}

void RealtimeChartView::setMaxPoints(int n)
{
    m_maxPoints = qMax(1, n);
}

int RealtimeChartView::maxPoints() const
{
    return m_maxPoints;
}

RealtimeChartView::ChartContext RealtimeChartView::makeChart(const QString &title,
                                                             const QString &yTitle,
                                                             QWidget *host)
{
    ChartContext ctx;

    ctx.m_chart = new QChart();
    ctx.m_chart->setTitle(title);
    ctx.m_chart->legend()->hide();   // 默认不给图例；延迟图在外面单独打开

    ctx.m_view = new QChartView(ctx.m_chart, host);
    ctx.m_view->setRenderHint(QPainter::Antialiasing);   // 不画锯齿，线好看很多

    ctx.m_axisX = new QValueAxis();
    ctx.m_axisX->setTitleText(QStringLiteral("秒"));
    ctx.m_axisX->setRange(0, 30);   // 给个初值，不然是 0~0 什么都看不见

    ctx.m_axisY = new QValueAxis();
    ctx.m_axisY->setTitleText(yTitle);
    ctx.m_axisY->setRange(0, 100);

    ctx.m_chart->addAxis(ctx.m_axisX, Qt::AlignBottom);
    ctx.m_chart->addAxis(ctx.m_axisY, Qt::AlignLeft);

    return ctx;
}

void RealtimeChartView::refreshChart(ChartContext &ctx, QLineSeries *series,
                                     QVector<QPointF> &points, double yMax)
{
    // ★ 为什么必须限长：不限的话跑一小时就是 3600 个点，QLineSeries 的绘制是 O(n)，
    //   QVector 也在无限长 —— 内存和帧率一起退化。默认 180 点（3 分钟）。
    while (points.size() > m_maxPoints) points.removeFirst();

    series->replace(points);   // 一次性替换整条曲线，一秒只重绘一次

    // Y 轴自适应：不让曲线跑出可视区，也不让它缩成一条扁线。×1.15 留头部空间。
    // ★ yMax 形参的真实语义是【下界】—— 函数内部还会从 points 里重算真实最大值。
    //   延迟图两条线共轴，所以调用方必须把"两条线一起算好的"值传进来，见 appendSample。
    for (const QPointF &p : points) yMax = qMax(yMax, p.y());
    ctx.m_axisY->setRange(0.0, qMax(10.0, yMax * 1.15));
}

void RealtimeChartView::resetOne(ChartContext &ctx, QLineSeries &series,
                                 QVector<QPointF> &points)
{
    points.clear();
    series.replace(points);         // 空 vector = 清空曲线
    ctx.m_axisX->setRange(0, 30);   // 轴范围也复位，免得上一轮的范围残留
    ctx.m_axisY->setRange(0, 100);
    ctx.m_view->update();           // 立刻重绘一次，否则要等下一个点才刷新
}

void RealtimeChartView::reset()
{
    // ★ 对外签名保持无参 —— 要清的东西全是本类私有的。
    //   （曾经想改成 reset(ChartContext&, QLineSeries&, ...) 来复用代码，但
    //     ChartContext 是 private 嵌套类型，MainWindow 根本构造不出来 → C2660。
    //     复用靠 private helper，不靠改对外签名。）
    resetOne(m_rpsctx, *m_rpsSeries, m_rpsPoints);
    resetOne(m_vuctx,  *m_vuSeries,  m_vuPoints);
    // 延迟图两条线共用一套轴：轴范围被复位两次只是重复劳动，结果一样
    resetOne(m_latctx, *m_avgSeries, m_avgPoints);
    resetOne(m_latctx, *m_p95Series, m_p95Points);

    m_t0Ms = 0;   // 下一轮的第一个点会重新定基准时间
}

void RealtimeChartView::appendSample(const WindowSample &sample)
{
    // ① 第一个点定基准时间。用 epochMs 直接当 X 的话，X 会是 1.7e12 这种天文数字，
    //    轴刻度显示成 1.7E+12，完全没法看。
    // ★ m_t0Ms 是【整个视图一份】的，不能塞进 ChartContext —— 否则三张图各自定基准，
    //   而 VU 有前向填充、可能晚一帧才收到数据，三条曲线的"第 5 秒"就不是同一时刻了。
    if (m_t0Ms == 0) m_t0Ms = sample.epochMs;

    // ② X = 相对秒数。只算一次、三张图共用 —— 这就是"三图横向对齐"的全部秘密。
    const double x = double(sample.epochMs - m_t0Ms) / 1000.0;

    // ③ 一份数据【扇出】到三处。
    //    ★ 注意这不是"给三个图适配数据" —— WindowSample 里本来就装齐了三张图要的字段，
    //      缺的只是分发。所以这个函数签名一个参数都不能加，也不能拆成三个公开方法：
    //      MainWindow 只需要知道"喂一秒数据"，不该知道内部有三张图。
    m_rpsPoints.append(QPointF(x, sample.rps));
    m_vuPoints.append(QPointF(x, sample.vus));   // vus 已是最近采样值（聚合器天然前向填充）
    m_avgPoints.append(QPointF(x, sample.avgDurationMs));
    m_p95Points.append(QPointF(x, sample.p95DurationMs));

    // ④ X 轴范围：先让最近的点挨着右边界（最小 30 秒，免得开头几秒被拉得太宽）
    const double maxX = qMax(30.0, x + 1.0);
    m_rpsctx.m_axisX->setRange(0.0, maxX);
    m_vuctx.m_axisX->setRange(0.0, maxX);
    m_latctx.m_axisX->setRange(0.0, maxX);

    // 跑超过 3 分钟后改成跟随式滚动窗口
    if (sample.epochMs - m_t0Ms > 3 * 60 * 1000) {
        const double minX = qMax(0.0, x - m_maxPoints);
        m_rpsctx.m_axisX->setRange(minX, x + 1.0);
        m_vuctx.m_axisX->setRange(minX, x + 1.0);
        m_latctx.m_axisX->setRange(minX, x + 1.0);
    }

    // ⑤ 刷新四条曲线。
    //    ★ 延迟图两条线【共用一套 Y 轴】—— 必须先把两条线的最大值一起算出来再传。
    //      分别传各自的 yMax，后一次 setRange 会覆盖前一次：高的那条被切到可视区外，
    //      矮的那条被压成贴底直线。（当前因为 p95 ≥ avg 恒成立才没显形。）
    const double latYMax = qMax(maxYOf(m_avgPoints), maxYOf(m_p95Points));

    refreshChart(m_rpsctx, m_rpsSeries, m_rpsPoints, sample.rps);
    refreshChart(m_vuctx,  m_vuSeries,  m_vuPoints,  double(sample.vus));
    refreshChart(m_latctx, m_avgSeries, m_avgPoints, latYMax);
    refreshChart(m_latctx, m_p95Series, m_p95Points, latYMax);

    // 自问一句：如果这一秒一个请求都没有，RPS 是多少？曲线会怎样？
    // （答案：0。曲线掉到 X 轴上 —— 这才是对的，RPS 为 0 就该显示 0。）
}
