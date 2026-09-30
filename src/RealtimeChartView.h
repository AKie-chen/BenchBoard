#pragma once

// ============================================================================
// RealtimeChartView —— 实时曲线面板
//
// 三张图（RPS / VU / 延迟）+ 一个 ChartContext
//   三张图要各装一遍「chart + view + axisX + axisY」，
//   于是抽成了下面的 ChartContext —— 这就是"分层是重构出来的"的最小例子。
//
//   ★ 抽的时候只收【每张图一份】的东西（chart / view / 两个轴）。
//     QLineSeries 和它的 points 是【每条线一份】且成对，留在外面 ——
//     延迟图有 avg + p95【两条】线却只有一个 m_latctx，就是这条判断法的反证。
//   ★ 颜色属于 series，归 series 的创建方（setColor 紧跟在 new QLineSeries 之后）。
//     反例：给 makeChart 加一个 lineColor 参数 —— 那是粒度混淆，
//     它想给"每条线一份"的东西上色，却挂在"每张图一份"的工厂函数上。
//
// ★★ Qt Charts 新手最容易栽的三处（前两处是环境，第三处是性能）★★
//
//   ① Qt6 里没有 QT_CHARTS_USE_NAMESPACE 了（那是 Qt5 的写法）。
//      直接 #include <QtCharts/QChartView> 就能用，类在全局命名空间。
//
//   ② CMake 里【两处】都要加 Charts，少一处就链接失败：
//        find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)
//        target_link_libraries(... Qt6::Widgets Qt6::Charts)
//
//   ③ 更新曲线【不要】clear() + 逐点 append()：
//      那样每来一个点触发一次重绘。正确做法是维护一个定长 QVector<QPointF>，
//      用 QLineSeries::replace() 一次性替换整条曲线，一秒只重绘一次。
//      （这和「回调频率 ≠ 显示频率」是同一条纪律换了战场）
//
// ★★ 第四个坑更致命：所有权 ★★
//   QChartView 内部是个 QGraphicsScene，它会 addItem(chart) 接管 chart；
//   chart->addSeries() / chart->addAxis() 又接管 series 和 axis。
//   所以这一条链上的对象【全部必须 new】—— 栈对象一析构，scene 手里就是悬垂指针。
//   实测 A/B：栈版 exit=139（SIGSEGV），堆版 exit=0。
//
// X 轴语义：本视图用「压测已进行的秒数」而不是日期时间 ——
//   QValueAxis 比 QDateTimeAxis 简单得多（不用管时区、格式串、跨度自适应），
//   而曲线本来就是相对的，用秒数反而更好读。
// ============================================================================

#include "types.h"

#include <QPointF>
#include <QVector>
#include <QWidget>

QT_BEGIN_NAMESPACE
class QChart;
class QChartView;
class QLineSeries;
class QValueAxis;
QT_END_NAMESPACE

class RealtimeChartView : public QWidget
{
    Q_OBJECT

    struct ChartContext;
public:
    explicit RealtimeChartView(QWidget *parent = nullptr);

    // 曲线横向可见窗口点数（默认 180，即最近 3 分钟）
    void setMaxPoints(int n);
    int  maxPoints() const;

    void reset();                      // 压测开始前清空曲线与缓冲

public slots:
    // 每秒由 MainWindow::onUiTick() 喂一个进来
    void appendSample(const WindowSample &sample);

private:
    void resetOne(ChartContext& ctx, QLineSeries& series, QVector<QPointF>& points);

    struct ChartContext {
        // --- 下面 5 个指针全部要 new（见头部注释第四个坑）---
        QChart      *m_chart     = nullptr;   // 图表本体（标题、图例、坐标轴集合）
        QChartView  *m_view      = nullptr;   // 把 chart 显示出来的 QWidget
        QValueAxis  *m_axisX     = nullptr;   // X：压测已进行秒数
        QValueAxis  *m_axisY     = nullptr;   // Y：RPS
    };

    // 构造图表
    ChartContext makeChart(const QString& title, const QString& yTitle, QWidget* host);

    void refreshChart(ChartContext& ctx, QLineSeries* series,
         QVector<QPointF>& points, double yMax);

    ChartContext m_rpsctx;      // RPS图表
    ChartContext m_vuctx;      // VU图表
    ChartContext m_latctx;      // 平均延迟图表

    QLineSeries *m_rpsSeries = nullptr;   // RPS 折线
    QLineSeries *m_vuSeries = nullptr;    // VU 折线
    QLineSeries *m_avgSeries = nullptr;   // 平均延迟折线
    QLineSeries *m_p95Series = nullptr;   // 95%延迟折线

    QVector<QPointF> m_rpsPoints;           // RPS 数据曲线
    QVector<QPointF> m_vuPoints;            // VU 数据曲线
    QVector<QPointF> m_avgPoints;           // 平均延迟数据曲线
    QVector<QPointF> m_p95Points;           // 95%延迟数据曲线

    qint64 m_t0Ms = 0;                       // 上次 appendSample() 的时间戳（毫秒）
    int m_maxPoints = 180;               // 超过它就删最老的点，否则内存和渲染都会退化
};
