// ============================================================================
// m5_chartcontext_probe —— ChartContext 的成员【为什么只能放指针】
//
// 背景（M5-Q1）：
//   题面问「ChartContext 里为什么只能放指针成员，不能放 QLineSeries 值成员？」
//   m4_stack_owner_probe 只测了【函数局部栈对象】那一种形态，答不了这道题 ——
//   ChartContext 是 RealtimeChartView 的【值成员】，它里面的成员如果是值对象，
//   析构会发生在「~RealtimeChartView → 成员逐个析构」这个时点，而不是"函数返回"。
//   本探针把 5 种布局分别建出来，一次只让【一个】成员变成值对象。
//
// 六个模式：
//   ptr-all      基线：4 个成员全是裸指针（= 真实设计）→ 看它为什么是安全的
//   val-chart    只有 m_chart 是值对象      → 预测：崩（scene 裸指针持有）
//   val-axis     只有两个轴是值对象         → 预测：不崩，轴从 chart 里消失
//   val-series   只有 series 是值对象       → 预测：不崩，线从 chart 里消失
//   val-view     只有 m_view 是值对象       → 预测：不崩，整块图消失
//   series-gap   单独的：QLineSeries 漏掉中间几秒的 x，线会不会【断开】？
//                （M5-Q2 的判据：如果只是"直连过去"，那就不是"断点"）
//
// 每个模式四段动作，用来定位崩在哪一步：
//   ① ctx 作用域结束（值成员在此析构）
//   ② show() + processEvents()      （第一次绘制 → 访问 chart 内部指针）
//   ③ resize() + processEvents()
//   ④ delete host
//
// 判据不只"崩没崩"：还要看【彩色像素】—— 图还在但线没了，也是坏结果。
// 用法：m5ctx.exe <mode>
// ============================================================================

#include <QAbstractSeries>
#include <QApplication>
#include <QChart>
#include <QChartView>          // ★ 别指望 <QChart> 把它带出来 —— 少了它就是 C2143
#include <QGridLayout>
#include <QImage>
#include <QLineSeries>
#include <QPainter>
#include <QValueAxis>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>

// ---------------------------------------------------------------------------
// 彩色像素计数（同 m5_chart_paint_probe）：轴/文字是灰阶，曲线是彩色
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
            if (qMax(r, qMax(g, b)) - qMin(r, qMin(g, b)) > 30) ++n;
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

// ---------------------------------------------------------------------------
// 装配：给 chart 挂一条 3 点的线（和 m4_stack_owner_probe 一致，便于对照）
// ---------------------------------------------------------------------------
static void assemble(QChart *chart, QLineSeries *series, QValueAxis *axisX, QValueAxis *axisY)
{
    chart->setTitle(QStringLiteral("请求速率 RPS"));
    chart->legend()->hide();
    chart->addSeries(series);                 // ① 先进 chart
    axisX->setTitleText(QStringLiteral("秒"));
    axisX->setRange(0, 30);
    axisY->setTitleText(QStringLiteral("req/s"));
    axisY->setRange(0, 100);
    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisX);                // ② 再绑轴
    series->attachAxis(axisY);
    series->append(0, 10);
    series->append(1, 20);
    series->append(2, 15);
}

// chart 里所有 series 的点数合计（用来判断"线还在不在"）
static int totalPoints(QChart *c)
{
    int n = 0;
    for (QAbstractSeries *s : c->series())
        if (auto *ls = qobject_cast<QLineSeries *>(s)) n += int(ls->count());
    return n;
}

// 五种布局，只有值成员的位置不同
struct CtxPtr       { QChart *m_chart=nullptr; QChartView *m_view=nullptr;
                      QValueAxis *m_axisX=nullptr; QValueAxis *m_axisY=nullptr; };
struct CtxValChart  { QChart  m_chart;         QChartView *m_view=nullptr;
                      QValueAxis *m_axisX=nullptr; QValueAxis *m_axisY=nullptr; };
struct CtxValAxis   { QChart *m_chart=nullptr; QChartView *m_view=nullptr;
                      QValueAxis m_axisX;       QValueAxis m_axisY; };
struct CtxValSeries { QChart *m_chart=nullptr; QChartView *m_view=nullptr;
                      QLineSeries m_series;     QValueAxis *m_axisX=nullptr;
                      QValueAxis *m_axisY=nullptr; };

// ---------------------------------------------------------------------------
// ① 基线：全指针
// ---------------------------------------------------------------------------
static QChartView *buildPtrAll(QWidget *host)
{
    QChart *c = nullptr; QChartView *v = nullptr;
    {
        CtxPtr ctx;
        ctx.m_chart = new QChart;
        ctx.m_view  = new QChartView(ctx.m_chart, host);
        ctx.m_axisX = new QValueAxis;
        ctx.m_axisY = new QValueAxis;
        auto *s = new QLineSeries;
        assemble(ctx.m_chart, s, ctx.m_axisX, ctx.m_axisY);
        c = ctx.m_chart; v = ctx.m_view;
    }   // ← CtxPtr 析构：4 个裸指针，析构【什么都不做】
    printf("  [after-scope] ctx 已析构  chart->axes()=%d  series=%d\n",
           int(c->axes().size()), int(c->series().size()));
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(v);
    return v;
}

// ---------------------------------------------------------------------------
// ② 只有 m_chart 是值对象
// ---------------------------------------------------------------------------
static QChartView *buildValChart(QWidget *host)
{
    QChartView *v = nullptr;
    {
        CtxValChart ctx;                                  // ← 栈上的 struct
        ctx.m_view  = new QChartView(&ctx.m_chart, host);
        ctx.m_axisX = new QValueAxis;
        ctx.m_axisY = new QValueAxis;
        auto *s = new QLineSeries;
        assemble(&ctx.m_chart, s, ctx.m_axisX, ctx.m_axisY);
        v = ctx.m_view;
    }   // ← ★ ~QChart 在这里跑：view 的 scene 手里还是那个裸指针
    printf("  [after-scope] ~QChart 已执行（没有访问 chart，访问就是崩）\n");
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(v);
    return v;
}

// ---------------------------------------------------------------------------
// ③ 只有两个轴是值对象
// ---------------------------------------------------------------------------
static QChartView *buildValAxis(QWidget *host)
{
    QChart *c = nullptr; QChartView *v = nullptr;
    {
        CtxValAxis ctx;
        ctx.m_chart = new QChart;
        ctx.m_view  = new QChartView(ctx.m_chart, host);
        auto *s = new QLineSeries;
        assemble(ctx.m_chart, s, &ctx.m_axisX, &ctx.m_axisY);
        c = ctx.m_chart; v = ctx.m_view;
    }   // ← ~QValueAxis ×2 在这里跑
    printf("  [after-scope] 两个轴已析构  chart->axes()=%d (建时是 2)  series=%d\n",
           int(c->axes().size()), int(c->series().size()));
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(v);
    return v;
}

// ---------------------------------------------------------------------------
// ④ 只有 series 是值对象
// ---------------------------------------------------------------------------
static QChartView *buildValSeries(QWidget *host)
{
    QChart *c = nullptr; QChartView *v = nullptr;
    {
        CtxValSeries ctx;
        ctx.m_chart = new QChart;
        ctx.m_view  = new QChartView(ctx.m_chart, host);
        ctx.m_axisX = new QValueAxis;
        ctx.m_axisY = new QValueAxis;
        assemble(ctx.m_chart, &ctx.m_series, ctx.m_axisX, ctx.m_axisY);
        c = ctx.m_chart; v = ctx.m_view;
    }   // ← ~QLineSeries 在这里跑
    printf("  [after-scope] series 已析构  chart->series()=%d (建时是 1)  points=%d (建时是 3)\n",
           int(c->series().size()), totalPoints(c));
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(v);
    return v;
}

// ---------------------------------------------------------------------------
// ⑤ 只有 m_view 是值对象 —— QChartView 需要 parent，只能建在作用域里
// ---------------------------------------------------------------------------
static QWidget *buildValView(QWidget *host)
{
    {
        QChart *chart = new QChart;
        QChartView view(chart, host);          // ← 栈上 QWidget
        auto *s = new QLineSeries;
        auto *ax = new QValueAxis;
        auto *ay = new QValueAxis;
        assemble(chart, s, ax, ay);
        auto *lay = new QVBoxLayout(host);
        lay->addWidget(&view);
    }   // ← ~QChartView：view 从 host 的 children 里摘牌，chart 随 scene 一起走
    printf("  [after-scope] view/chart 都随作用域没了  host->children()=%d\n",
           int(host->children().size()));
    return nullptr;
}

// ---------------------------------------------------------------------------
// ⑥ QLineSeries 中间漏几秒的 x —— 线是【断开】还是【直连过去】？
// ---------------------------------------------------------------------------
static void probeSeriesGap()
{
    printf("\n=== series-gap：x 从 3 直接跳到 7（漏掉第 4/5/6 秒）===\n");

    QWidget host;
    host.resize(520, 360);

    auto *chart = new QChart;
    chart->legend()->hide();
    auto *s = new QLineSeries;
    auto *ay = new QValueAxis;
    auto *ax = new QValueAxis;
    chart->addSeries(s);
    ax->setRange(0, 10);
    ay->setRange(0, 100);
    chart->addAxis(ax, Qt::AlignBottom);
    chart->addAxis(ay, Qt::AlignLeft);
    s->attachAxis(ax);
    s->attachAxis(ay);

    // 对照组：x 连续 1..3（每列都有点）
    // 目标组：x = 1,2,3,7,8,9 —— 4/5/6 秒没有数据点
    s->append(1, 50);
    s->append(2, 50);
    s->append(3, 50);
    s->append(7, 50);
    s->append(8, 50);
    s->append(9, 50);

    auto *v = new QChartView(chart, &host);
    v->setRenderHint(QPainter::Antialiasing);
    auto *lay = new QVBoxLayout(&host);
    lay->addWidget(v);

    host.show();
    QApplication::processEvents();

    const QImage img = renderWidget(v);

    // 用 mapToPosition 把"第 5 秒、y=50"这个数据点换算成像素位置
    const QPointF scenePt = chart->mapToPosition(QPointF(5.0, 50.0), s);
    const QPoint  vp      = v->mapFromScene(scenePt);

    // 在 vp.x 附近取一条竖带，数彩色像素：>0 说明线从这里穿过去了
    int bandPx = 0;
    const int x0 = qMax(0, vp.x() - 4), x1 = qMin(img.width() - 1, vp.x() + 4);
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = x0; x <= x1; ++x) {
            const QRgb p = row[x];
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            if (qMax(r, qMax(g, b)) - qMin(r, qMin(g, b)) > 30) ++bandPx;
        }
    }
    printf("  第 5 秒（gap 正中）映射到像素 x=%d\n", vp.x());
    printf("  该处竖带 x∈[%d,%d] 的彩色像素 = %d\n", x0, x1, bandPx);
    printf("  → 判读：%s\n", bandPx > 0
           ? "线【穿过了空档】（QLineSeries 按顺序连点，不会断开）"
           : "线在这里【断了】（空档真的没画）");
    printf("  整图彩色像素 = %d\n", countColorPixels(img));
    img.save(QStringLiteral("series_gap.png"));
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // ★ 无缓冲：本探针有意触发崩溃
    QApplication app(argc, argv);

    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("ptr-all");
    printf("=== mode = %s ===\n", qPrintable(mode));

    if (mode == QStringLiteral("series-gap")) { probeSeriesGap(); return 0; }

    auto *host = new QWidget;
    host->resize(520, 360);

    QChartView *view = nullptr;
    if      (mode == QStringLiteral("ptr-all"))    view = buildPtrAll(host);
    else if (mode == QStringLiteral("val-chart"))  view = buildValChart(host);
    else if (mode == QStringLiteral("val-axis"))   view = buildValAxis(host);
    else if (mode == QStringLiteral("val-series")) view = buildValSeries(host);
    else if (mode == QStringLiteral("val-view")) { buildValView(host); view = nullptr; }
    else { printf("unknown mode: %s\n", qPrintable(mode)); return 2; }

    printf("  [main] ① 作用域已结束，view=%p\n", static_cast<void *>(view));

    host->show();
    app.processEvents();
    printf("  [main] ② show + processEvents 通过（第一次绘制没崩）\n");
    if (view) printf("         彩色像素 = %d\n", countColorPixels(renderWidget(host)));

    host->resize(560, 400);
    app.processEvents();
    printf("  [main] ③ resize 重绘通过\n");

    delete host;
    printf("  [main] ④ delete host 通过 —— 本模式【未复现】崩溃\n");
    return 0;
}
