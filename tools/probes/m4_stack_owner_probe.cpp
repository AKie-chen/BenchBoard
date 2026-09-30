// ============================================================================
// m4_stack_owner_probe —— Qt Charts 的所有权链：到底【哪一个】对象的栈化会崩？
//
// 背景（M4-Q1 的追问）：
//   RealtimeChartView.cpp 的构造函数里，chart / series / axisX / axisY 全部 new。
//   题面追问："如果【只把 axisY】写成栈对象，会崩吗？为什么？"
//
//   已实测的事实（docs/02 3.1 + m4_skeleton_check）：
//     全部栈 → 崩（Windows exit=127），崩在"函数返回、栈对象析构"那一刻。
//   但"只栈一个"会怎样，以及"崩在哪一步"—— 未验证。
//   本探针把所有权链逐段切开，一次只让一个对象上栈。
//
// 六个模式：
//   heap         全堆（基线）
//   stack-all    全栈（复现 3.1 的崩溃）
//   stack-chart  只 chart 栈，series/两个轴堆
//   stack-series 只 series 栈
//   stack-axisY  只 axisY 栈  ← 题面追问的那个
//   stack-axisX  只 axisX 栈（和 axisY 对照，确认是不是"哪个轴都一样"）
//
// 每个模式都经过三段动作，用来定位崩在哪一步：
//   ① build 返回（栈对象在此析构）
//   ② show() + processEvents()（触发第一次绘制 → 访问 chart 内部指针）
//   ③ host->resize() + processEvents()（再绘一次）
//   ④ delete host（销毁 view → scene clear）
//
// 用法：m4_stack_owner.exe <mode>
// ============================================================================

#include <QApplication>
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#include <QLayout>
#include <QValueAxis>
#include <QWidget>

#include <cstdio>

// ---------------------------------------------------------------------------
// 装配公共部分：把一条 3 点的线挂好
// ---------------------------------------------------------------------------
static void assemble(QChart *chart, QLineSeries *series, QValueAxis *axisX, QValueAxis *axisY)
{
    chart->setTitle(QStringLiteral("请求速率 RPS"));
    chart->legend()->hide();
    chart->addSeries(series);

    axisX->setTitleText(QStringLiteral("秒"));
    axisX->setRange(0, 30);
    axisY->setTitleText(QStringLiteral("req/s"));
    axisY->setRange(0, 100);

    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisX);
    series->attachAxis(axisY);

    series->append(0, 10);
    series->append(1, 20);
    series->append(2, 15);
}

static void report(const char *tag, QChart *chart, QLineSeries *series)
{
    printf("  [%s] chart->axes()=%d  series->count()=%d\n",
           tag, static_cast<int>(chart->axes().size()), static_cast<int>(series->count()));
}

// ---------------------------------------------------------------------------
// ① 全堆（基线）
// ---------------------------------------------------------------------------
static QChartView *buildHeap(QWidget *host)
{
    auto *chart  = new QChart;
    auto *series = new QLineSeries;
    auto *axisX  = new QValueAxis;
    auto *axisY  = new QValueAxis;
    auto *view   = new QChartView(chart, host);

    assemble(chart, series, axisX, axisY);
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    report("build-heap", chart, series);
    return view;
}

// ---------------------------------------------------------------------------
// ② 全栈 —— 复现 docs/02 3.1
// ---------------------------------------------------------------------------
static QChartView *buildStackAll(QWidget *host)
{
    QChart     chart;             // ← 栈
    QLineSeries series;           // ← 栈
    QValueAxis axisX;             // ← 栈
    QValueAxis axisY;             // ← 栈

    auto *view = new QChartView(&chart, host);
    assemble(&chart, &series, &axisX, &axisY);
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    report("build-stack-all", &chart, &series);
    printf("  [build-stack-all] 即将返回 —— 四个栈对象就要析构\n");
    return view;
}   // ← 四个栈对象在这里析构

// ---------------------------------------------------------------------------
// ③ 只 chart 栈
// ---------------------------------------------------------------------------
static QChartView *buildStackChart(QWidget *host)
{
    QChart chart;                 // ← 栈
    auto *series = new QLineSeries;
    auto *axisX  = new QValueAxis;
    auto *axisY  = new QValueAxis;

    auto *view = new QChartView(&chart, host);
    assemble(&chart, series, axisX, axisY);
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    report("build-stack-chart", &chart, series);
    printf("  [build-stack-chart] 即将返回 —— 栈上的 chart 就要析构\n");
    return view;
}

// ---------------------------------------------------------------------------
// ④ 只 series 栈
// ---------------------------------------------------------------------------
static QChartView *buildStackSeries(QWidget *host)
{
    auto *chart  = new QChart;
    QLineSeries series;           // ← 栈
    auto *axisX  = new QValueAxis;
    auto *axisY  = new QValueAxis;

    auto *view = new QChartView(chart, host);
    assemble(chart, &series, axisX, axisY);
    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    report("build-stack-series", chart, &series);
    printf("  [build-stack-series] 即将返回 —— 栈上的 series 就要析构\n");
    return view;
}

// ---------------------------------------------------------------------------
// ⑤ 只 axisY 栈  ← M4-Q1 追问的那个
// ---------------------------------------------------------------------------
static QChartView *buildStackAxisY(QWidget *host)
{
    auto *chart  = new QChart;
    auto *series = new QLineSeries;
    auto *axisX  = new QValueAxis;
    auto *view   = new QChartView(chart, host);

    chart->setTitle(QStringLiteral("请求速率 RPS"));
    chart->legend()->hide();
    chart->addSeries(series);
    axisX->setTitleText(QStringLiteral("秒"));
    axisX->setRange(0, 30);
    chart->addAxis(axisX, Qt::AlignBottom);
    series->attachAxis(axisX);

    {
        QValueAxis axisY;         // ← 栈，且只在这一层作用域里
        axisY.setTitleText(QStringLiteral("req/s"));
        axisY.setRange(0, 100);
        chart->addAxis(&axisY, Qt::AlignLeft);
        series->attachAxis(&axisY);
        series->append(0, 10);
        series->append(1, 20);
        series->append(2, 15);
        printf("  [build-stack-axisY] axisY 还在作用域内：chart->axes()=%d\n",
               static_cast<int>(chart->axes().size()));
    }   // ← axisY 在这里析构

    printf("  [build-stack-axisY] axisY 已析构：chart->axes()=%d  "
           "（若仍是 2，说明 chart 的轴列表里留着悬垂指针）\n",
           static_cast<int>(chart->axes().size()));

    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    return view;
}

// ---------------------------------------------------------------------------
// ⑥ 只 axisX 栈（对照，确认是不是"哪个轴都一样"）
// ---------------------------------------------------------------------------
static QChartView *buildStackAxisX(QWidget *host)
{
    auto *chart  = new QChart;
    auto *series = new QLineSeries;
    auto *axisY  = new QValueAxis;
    auto *view   = new QChartView(chart, host);

    chart->setTitle(QStringLiteral("请求速率 RPS"));
    chart->legend()->hide();
    chart->addSeries(series);
    axisY->setTitleText(QStringLiteral("req/s"));
    axisY->setRange(0, 100);
    chart->addAxis(axisY, Qt::AlignLeft);
    series->attachAxis(axisY);

    {
        QValueAxis axisX;         // ← 栈
        axisX.setTitleText(QStringLiteral("秒"));
        axisX.setRange(0, 30);
        chart->addAxis(&axisX, Qt::AlignBottom);
        series->attachAxis(&axisX);
        series->append(0, 10);
        series->append(1, 20);
        series->append(2, 15);
    }

    printf("  [build-stack-axisX] axisX 已析构：chart->axes()=%d\n",
           static_cast<int>(chart->axes().size()));

    auto *lay = new QVBoxLayout(host);
    lay->addWidget(view);
    return view;
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    // ★ 无缓冲：本探针有意触发崩溃，缓冲区被吞掉就什么都看不到了
    setvbuf(stdout, nullptr, _IONBF, 0);

    QApplication app(argc, argv);
    const QString mode = argc > 1 ? QString::fromLatin1(argv[1]) : QStringLiteral("heap");
    printf("=== mode = %s ===\n", qPrintable(mode));

    auto *host = new QWidget;
    host->resize(480, 320);

    QChartView *view = nullptr;
    if (mode == QStringLiteral("heap"))              view = buildHeap(host);
    else if (mode == QStringLiteral("stack-all"))    view = buildStackAll(host);
    else if (mode == QStringLiteral("stack-chart"))  view = buildStackChart(host);
    else if (mode == QStringLiteral("stack-series")) view = buildStackSeries(host);
    else if (mode == QStringLiteral("stack-axisY"))  view = buildStackAxisY(host);
    else if (mode == QStringLiteral("stack-axisX"))  view = buildStackAxisX(host);
    else { printf("unknown mode: %s\n", qPrintable(mode)); return 2; }

    printf("  [main] ① build 已返回（栈对象如有则已析构），view=%p\n", static_cast<void *>(view));

    host->show();
    app.processEvents();
    printf("  [main] ② show + processEvents 通过（第一次绘制没崩）\n");

    host->resize(520, 360);
    app.processEvents();
    printf("  [main] ③ resize 重绘通过\n");

    delete host;
    printf("  [main] ④ delete host 通过 —— 本次【未复现】崩溃\n");
    return 0;
}
