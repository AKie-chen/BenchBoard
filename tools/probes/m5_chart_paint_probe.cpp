// ============================================================================
// m5_chart_paint_probe —— 「图上有轴、没有曲线」的像素级定位
//
// 判据不是"看着对不对"，而是三个可数的量：
//   ① series->attachedAxes().size()   —— 轴到底绑上没有（Qt 只在成功时才填它）
//   ② chart->series().size()          —— series 到底进没进 chart
//   ③ 渲染图里的【彩色像素数】         —— 轴/网格/文字都是灰阶(R≈G≈B)，
//                                        曲线是 QLineSeries 的默认蓝 → 彩色
//                                        彩色像素 ≈ 0 就是"一条线都没画"
//
// Part A 做顺序 A/B 对照（裸 QChart，排除一切其它因素）：
//   A 组：attachAxis 在 addSeries 【之前】（= 当前 src 的写法）
//   B 组：addSeries 在 attachAxis 【之前】（= Qt 文档要求的顺序）
//
// Part B 体检真实的 RealtimeChartView：喂 10 个样本，逐张图报数。
// ============================================================================

#include "RealtimeChartView.h"
#include "types.h"

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QGridLayout>
#include <QImage>
#include <QLineSeries>
#include <QMessageLogContext>
#include <QPainter>
#include <QString>
#include <QStringList>
#include <QValueAxis>
#include <QVector>

#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// qWarning 捕获 —— Qt 在 attachAxis 失败时会 qWarning，不抓就看不见
// ---------------------------------------------------------------------------
static QStringList g_warnings;

static void captureHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    const char *tag = "?";
    switch (type) {
    case QtDebugMsg:    tag = "DEBUG";   break;
    case QtInfoMsg:     tag = "INFO";    break;
    case QtWarningMsg:  tag = "WARNING"; break;
    case QtCriticalMsg: tag = "CRIT";    break;
    case QtFatalMsg:    tag = "FATAL";   break;
    }
    const QString line = QStringLiteral("[%1] %2").arg(QString::fromLatin1(tag), msg);
    g_warnings.append(line);
    std::fprintf(stdout, "        qWarning: %s\n", qPrintable(line));
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 彩色像素计数：|max(R,G,B) - min(R,G,B)| > 阈值 视为"彩色"
// 轴、刻度、标题文字都是灰阶 → 不计入。曲线是蓝色 → 计入。
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

// ---------------------------------------------------------------------------
// Part A：裸 QChart 顺序对照
// ---------------------------------------------------------------------------
struct OrderCase {
    QString label;
    bool    addSeriesFirst = false;
    bool    attX = false;
    bool    attY = false;
    int     attachedAfter = 0;
    int     pointsN = 0;
    int     colorPx = 0;
};

static OrderCase runOrderCase(const QString &label, bool addSeriesFirst)
{
    OrderCase c;
    c.label = label;
    c.addSeriesFirst = addSeriesFirst;

    QChart *chart = new QChart();
    chart->setTitle(label);
    chart->legend()->hide();

    QLineSeries *s = new QLineSeries();

    QValueAxis *ax = new QValueAxis();
    ax->setTitleText(QStringLiteral("秒"));
    ax->setRange(0, 10);
    QValueAxis *ay = new QValueAxis();
    ay->setTitleText(QStringLiteral("req/s"));
    ay->setRange(0, 100);
    chart->addAxis(ax, Qt::AlignBottom);
    chart->addAxis(ay, Qt::AlignLeft);

    if (addSeriesFirst) {
        chart->addSeries(s);                 // ← 先入 chart
        c.attX = s->attachAxis(ax);          // ← 再绑轴
        c.attY = s->attachAxis(ay);
    } else {
        c.attX = s->attachAxis(ax);          // ← 先绑轴（series 还没 chart）
        c.attY = s->attachAxis(ay);
        chart->addSeries(s);                 // ← 再入 chart
    }

    // 造 6 个点，形状明显（避免和轴线混淆）
    QVector<QPointF> pts;
    for (int i = 0; i < 6; ++i)
        pts.append(QPointF(i * 1.6, 15.0 + i * 13.0));
    s->replace(pts);
    c.pointsN = int(s->count());
    c.attachedAfter = int(s->attachedAxes().size());

    QChartView *v = new QChartView(chart);
    v->setRenderHint(QPainter::Antialiasing);
    v->resize(520, 360);
    const QImage img = renderWidget(v);
    c.colorPx = countColorPixels(img);
    img.save(label.startsWith(QLatin1Char('A')) ? QStringLiteral("A_group_attachFirst.png")
                                                 : QStringLiteral("B_group_addSeriesFirst.png"));

    delete v;   // 释放整条链（chart 由 view 持有，series/axis 由 chart 持有）
    return c;
}

// ---------------------------------------------------------------------------
// Part C：按 Qt 要求的顺序重建「三图 + 延迟图双线」，验证修复方案真的有效
//         （等价于"用户改完之后应该长什么样"的可执行规格）
// ---------------------------------------------------------------------------
static void probeCorrectBuild()
{
    QWidget host;
    host.resize(1000, 700);
    QGridLayout *lay = new QGridLayout(&host);

    struct Fig {
        QChart *chart = nullptr;
        QChartView *view = nullptr;
        QValueAxis *ax = nullptr;
        QValueAxis *ay = nullptr;
    };
    Fig figs[3];
    const QString titles[3]  = {QStringLiteral("请求速率 RPS"), QStringLiteral("虚拟用户数"), QStringLiteral("响应时间")};
    const QString yTitles[3] = {QStringLiteral("req/s"), QStringLiteral("count"), QStringLiteral("ms")};

    for (int i = 0; i < 3; ++i) {
        figs[i].chart = new QChart();
        figs[i].chart->setTitle(titles[i]);
        figs[i].chart->legend()->hide();
        figs[i].view = new QChartView(figs[i].chart);
        figs[i].view->setRenderHint(QPainter::Antialiasing);
        figs[i].ax = new QValueAxis();
        figs[i].ax->setTitleText(QStringLiteral("秒"));
        figs[i].ax->setRange(0, 30);
        figs[i].ay = new QValueAxis();
        figs[i].ay->setTitleText(yTitles[i]);
        figs[i].ay->setRange(0, 100);
        figs[i].chart->addAxis(figs[i].ax, Qt::AlignBottom);
        figs[i].chart->addAxis(figs[i].ay, Qt::AlignLeft);
    }
    lay->addWidget(figs[0].view, 0, 0);
    lay->addWidget(figs[1].view, 0, 1);
    lay->addWidget(figs[2].view, 1, 0, 1, 2);

    // 4 条 series，每条都是【先 addSeries 再 attachAxis】
    QLineSeries *rps = new QLineSeries(); figs[0].chart->addSeries(rps); rps->attachAxis(figs[0].ax); rps->attachAxis(figs[0].ay);
    QLineSeries *vu  = new QLineSeries(); figs[1].chart->addSeries(vu);  vu->attachAxis(figs[1].ax);  vu->attachAxis(figs[1].ay);
    QLineSeries *avg = new QLineSeries(); figs[2].chart->addSeries(avg); avg->attachAxis(figs[2].ax); avg->attachAxis(figs[2].ay);
    QLineSeries *p95 = new QLineSeries(); figs[2].chart->addSeries(p95); p95->attachAxis(figs[2].ax); p95->attachAxis(figs[2].ay);

    QVector<QPointF> pr, pv, pa, pp;
    const qint64 base = 1758700000000LL;
    for (int i = 1; i <= 36; ++i) {          // 同 Part B：喂满横轴再判像素
        const double x = double(i);
        const int    rc = 1150 + i * 12;
        pr.append(QPointF(x, double(rc)));
        pv.append(QPointF(x, 8.0));
        pa.append(QPointF(x, 3.2 + i * 0.1));
        pp.append(QPointF(x, 7.4 + i * 0.25));
    }
    rps->replace(pr);
    vu->replace(pv);
    avg->replace(pa);
    // ★ Y 轴自适应 —— 等价于 refreshChart() 里那段「遍历 points 取 maxY × 1.15」。
    //   不做这一步：数据 1150~1270 落在 0~100 的轴外面，曲线被画到画布之外，
    //   表现和「没绑轴」一模一样（彩色像素 = 0）—— 这是第二类「有图无线」的成因。
    for (int i = 0; i < 3; ++i) {
        double m = 0.0;
        for (QAbstractSeries *s : figs[i].chart->series())
            if (auto *ls = qobject_cast<QLineSeries *>(s))
                for (const QPointF &p : ls->points())
                    m = qMax(m, p.y());
        figs[i].ay->setRange(0.0, qMax(10.0, m * 1.15));
    }
    // ★ 延迟图双线共用一条 Y 轴 → 范围必须【把两条线一起算】，否则后一次 setRange 会盖掉前一次
    p95->replace(pp);
    for (int i = 0; i < 3; ++i)
        figs[i].ax->setRange(0.0, 37.0);      // 36 个点 → 让曲线横跨整幅

    host.show();
    QApplication::processEvents();

    std::printf("\n=== Part C：正确顺序重建三图（= 修好后应该长什么样）===\n");
    std::printf("%-18s %8s %6s %10s %10s %8s\n",
                "chart 标题", "series数", "点数", "绑定轴数", "彩色像素", "判定");
    std::printf("%s\n", QString(80, '-').toUtf8().constData());
    const QList<QChartView *> views = host.findChildren<QChartView *>();
    int k = 0;
    for (QChartView *v : views) {
        QChart *ch = v->chart();
        int pts = 0, att = 0;
        for (QAbstractSeries *s : ch->series()) {
            if (auto *ls = qobject_cast<QLineSeries *>(s)) {
                pts += int(ls->count());
                att += int(ls->attachedAxes().size());
            }
        }
        v->resize(480, 320);
        const QImage img = renderWidget(v);
        const int colorPx = countColorPixels(img);
        const QString f = QStringLiteral("C_correct_%1.png").arg(++k);
        img.save(f);
        std::printf("%-18s %8d %6d %10d %10d %8s\n",
                    qPrintable(ch->title()), int(ch->series().size()),
                    pts, att, colorPx, (colorPx > 200 ? "OK" : "FAIL"));
    }
}

// ---------------------------------------------------------------------------
// Part B：真实 RealtimeChartView 逐图体检
// ---------------------------------------------------------------------------
static void probeRealView()
{
    RealtimeChartView rv;
    rv.resize(1000, 700);
    rv.show();

    // ★ 喂 36 个点而不是 10 个：X 轴初值固定在 30 秒（appendSample 里 qMax(30.0, x+1.0)），
    //   只喂 10 个点的话曲线只占左侧 1/3 幅面，彩色像素必然少 —— 那会让
    //   "像素 > 200" 这条判据误报成 FAIL（实测 153 / 194，而图上明明有线）。
    //   喂满横轴，判据才是在测"画不画得出线"，而不是在测"曲线碰巧有多长"。
    const qint64 base = 1758700000000LL;   // 固定的基准时间，保证可复现
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
        rv.appendSample(w);
        QApplication::processEvents();
    }

    const QList<QChartView *> views = rv.findChildren<QChartView *>();
    std::printf("\n=== Part B：真实 RealtimeChartView 里找到 %lld 个 QChartView ===\n",
                static_cast<long long>(views.size()));
    std::printf("%-22s %8s %6s %10s %10s %12s\n",
                "chart 标题", "series数", "点数", "绑定轴数", "彩色像素", "判定");
    std::printf("%s\n", QString(90, '-').toUtf8().constData());

    int idx = 0;
    for (QChartView *v : views) {
        QChart *ch = v->chart();
        const int nSeries = ch ? int(ch->series().size()) : -1;
        const int nAxes   = ch ? int(ch->axes().size()) : -1;
        int totalPoints = 0, maxAttached = -1, totalAttached = 0;
        if (ch) {
            for (QAbstractSeries *s : ch->series()) {
                if (auto *ls = qobject_cast<QLineSeries *>(s)) {
                    totalPoints += int(ls->count());
                    const int a = int(ls->attachedAxes().size());
                    totalAttached += a;
                    maxAttached = qMax(maxAttached, a);
                }
            }
        }
        v->resize(480, 320);
        const int colorPx = countColorPixels(renderWidget(v));

        // 判定：有数据点 + 轴绑上了 + 真的画出了彩色像素
        const bool ok = (totalPoints > 0) && (maxAttached >= 2) && (colorPx > 200);
        const char *verdict = ok ? "OK" : "FAIL";

        std::printf("%-22s %8d %6d %10d %10d %12s\n",
                    ch ? qPrintable(ch->title()) : "(null)",
                    nSeries, totalPoints, totalAttached, colorPx, verdict);
        if (!ok) {
            if (nSeries <= 0)        std::printf("        └─ 原因：chart 里一个 series 都没有\n");
            else if (totalPoints == 0) std::printf("        └─ 原因：series 没有点\n");
            else if (maxAttached < 2)  std::printf("        └─ 原因：series 没有绑上轴（attachAxis 失败）\n");
            else                       std::printf("        └─ 原因：点、轴都在，但渲染不出彩色像素\n");
            if (nAxes >= 0) std::printf("        └─ 该 chart 的轴总数 = %d\n", nAxes);
        }
        ++idx;

        // 把每张图存成 png，肉眼复核
        renderWidget(v).save(QStringLiteral("paint_probe_view%1.png").arg(idx));
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);   // ★ 无缓冲，异常终止也不丢输出
    QApplication app(argc, argv);
    qInstallMessageHandler(captureHandler);

    std::printf("========== Part A：attachAxis / addSeries 顺序 A/B ==========\n\n");

    const OrderCase a = runOrderCase(QStringLiteral("A 组：先 attachAxis 后 addSeries"), false);
    std::printf("\n");
    const OrderCase b = runOrderCase(QStringLiteral("B 组：先 addSeries 后 attachAxis"), true);

    std::printf("\n%-34s %6s %6s %10s %10s %10s\n",
                "组", "attX", "attY", "入chart后", "点数", "彩色像素");
    std::printf("%s\n", QString(90, '-').toUtf8().constData());
    for (const OrderCase &c : {a, b}) {
        std::printf("%-34s %6s %6s %10d %10d %10d\n",
                    qPrintable(c.label),
                    c.attX ? "true" : "false",
                    c.attY ? "true" : "false",
                    c.attachedAfter, c.pointsN, c.colorPx);
    }
    std::printf("\n判据：彩色像素 > 200 才算「真的画出了曲线」。\n");

    probeRealView();
    probeCorrectBuild();

    std::printf("\n=== 本次捕获到的 qWarning（%lld 条）===\n",
                static_cast<long long>(g_warnings.size()));
    for (const QString &w : g_warnings)
        std::printf("  %s\n", w.toUtf8().constData());
    std::fflush(stdout);
    return 0;
}
