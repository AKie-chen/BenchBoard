#include "MainWindow.h"

#include "K6Engine.h"            // config.k6Path 用它的 detectK6Path() 探测引擎位置
#include "MetricsAggregator.h"   // onUiTick 里要调 metrics()->takeWindow() / stats()
#include "RealtimeChartView.h"   // 建图表区要用完整类型
#include "TestOrchestrator.h"    // 界面唯一的后端入口

#include <QComboBox>
#include <QCoreApplication>    // applicationDirPath()：报告目录与示例脚本按 exe 位置推导
#include <QDir>
#include <QDockWidget>     // 暂时不用（预留：脚本列表）
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>     // verticalHeader() / horizontalHeader()
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStatusBar>
#include <QTableWidget>        // buildUi 里要 new，必须看到完整定义
#include <QTableWidgetItem>    // 往格子里填字
#include <QTimer>
#include <QVBoxLayout>

namespace {

// 报告输出目录 —— 界面负责给出"这一次跑在哪"，其余路径推导归编排层与引擎。
// ★ 跟可执行文件走（<exe 同级>/reports），而不是写死某个盘符：
//   换机器、换盘符、换 clone 路径都不用改代码，它也不会去污染源码目录。
//   目录不存在由编排层的 mkpath 创建（见 TestOrchestrator::startTest）。
const QString kReportsDir = QCoreApplication::applicationDirPath()
                          + QStringLiteral("/reports");

// 汇总面板的行号：第一列是"指标"，第二列是"数值"
constexpr int kRowRequests = 0;   // 请求总数
constexpr int kRowFailures = 1;   // 失败数
constexpr int kRowAvg      = 2;   // 平均耗时
constexpr int kRowP95      = 3;   // p95
constexpr int kRowP99      = 4;   // p99
constexpr int kRowSource   = 5;   // 数据来源（k6 汇总 / 实时兜底）
constexpr int kSummaryRows = 6;

}  // namespace

// ============================================================================
// MainWindow —— 界面装配与信号接线
//
//   buildUi() 摆控件 / connectSignals() 接线 / 三个动作槽（开始·停止·导出）/
//   onUiTick() 每秒心跳 / 编排层四个信号的接收槽
//
// ★ 图表对象的分配纪律（已有 A/B 实测）：QChart / QLineSeries / QValueAxis
//   必须【堆分配】。QChartView 的 QGraphicsScene 会接管 chart，
//   chart->addSeries/addAxis 也接管所有权。栈对象在函数返回时析构
//   → scene 持悬垂指针 → 进程直接死。实测：栈版 exit=139（SIGSEGV），堆版 exit=0。
//
// ★ 曲线更新纪律：【不要「clear() + 逐点 append()」】—— 那样每加一个点就触发
//   一次重绘。正确做法是维护一个定长 QVector<QPointF>，用 QLineSeries::replace()
//   一次性换掉整条曲线 —— 反正我们是 1 秒才来一个点。
// ============================================================================

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // 建立编排层（唯一后端入口）。
    // ★ 必须写在 buildUi() / connectSignals() 【之前】——
    //   connectSignals() 里要 connect 它的信号，那一刻它不能是 nullptr，
    //   否则 Qt 只打印 "QObject::connect: invalid null parameter" 就静默跳过。
    m_orchestrator = new TestOrchestrator(this);

    // --- 每秒一跳的 QTimer ---
    // 只 new + setInterval，不 start() —— 什么时候开始跳由"这一轮真的跑起来了"决定
    // （在构造函数里就 start() 的话，压测还没跑状态栏就一直在刷 0）。
    m_uiTimer = new QTimer(this);
    m_uiTimer->setInterval(1000);      // 1 秒。这个值决定状态栏跳多快

    buildUi();

    // connectSignals() 必须在构造函数里调用，
    // 漏了这行【不会有任何报错】，只是按钮点了没反应。
    connectSignals();
}

void MainWindow::buildUi()
{
    // --- 界面骨架 ---
    QWidget *central = new QWidget(this);
    setCentralWidget(central);

    // ★ 记住这条：布局必须 new。写成 QVBoxLayout root(central); 也能编译，
    //   但函数一返回它就析构了，控件会瞬间失去布局 —— 界面变成一堆叠在一起的散件。
    QVBoxLayout *mainLayout = new QVBoxLayout(central);

    // 配置区组件
    QGroupBox *configGroup = new QGroupBox(QStringLiteral("压测配置"), central);
    QFormLayout *formLayout = new QFormLayout(configGroup);

    formLayout->addRow(QStringLiteral("目标 URL"), m_urlEdit = new QLineEdit(configGroup));

    formLayout->addRow(QStringLiteral("并发 VU"), m_vusSpin = new QSpinBox(configGroup));
    m_vusSpin->setRange(1, 1000);

    formLayout->addRow(QStringLiteral("时长"), m_durationCombo = new QComboBox(configGroup));
    m_durationCombo->addItem(QStringLiteral("10 秒"),    QStringLiteral("10s"));
    m_durationCombo->addItem(QStringLiteral("30 秒"),    QStringLiteral("30s"));
    m_durationCombo->addItem(QStringLiteral("1 分钟"),   QStringLiteral("60s"));

    mainLayout->addWidget(configGroup);

    // 按钮行（addStretch 让按钮靠左，否则按钮会被拉满整行）
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    buttonLayout->addWidget(m_startButton = new QPushButton(QStringLiteral("开始压测")));
    buttonLayout->addWidget(m_stopButton = new QPushButton(QStringLiteral("停止")));
    buttonLayout->addWidget(m_exportButton = new QPushButton(QStringLiteral("导出报告")));
    m_stopButton->setEnabled(false);
    m_exportButton->setEnabled(false);
    buttonLayout->addStretch();
    mainLayout->addLayout(buttonLayout);

    // --- 实时图表区（三张图）---
    // 摆在日志区【之上】，两者各占一半。
    //
    //   ★ 拉伸因子给 1、日志区也是 1 → 窗口拉高时两块各分一半。
    //     （M1 时日志区是唯一的 1，所以它独占了全部额外高度；现在它有个伴了。）
    //   ★ 顺序：这两行必须写在日志区的 addWidget 【之前】，
    //     否则图表会跑到日志区下面去 —— 布局是按 addWidget 的调用顺序排的。
    m_chartView = new RealtimeChartView(central);
    mainLayout->addWidget(m_chartView, 1);

    // --- 汇总面板 ---
    // 摆在三图【之下】、日志区【之上】。拉伸因子给 0 —— 它只要自己那点固定高度，
    // 多余的高度留给图表和日志区。
    m_summaryTable = new QTableWidget(central);
    m_summaryTable->setColumnCount(2);
    m_summaryTable->setHorizontalHeaderLabels(
        {QStringLiteral("指标"), QStringLiteral("数值")});
    // ★ setRowCount() 必须在任何 setItem() 【之前】—— 少了这一行，
    //   setItem() 会被【静默丢弃】，表格永远是空的，而且零报错。
    m_summaryTable->setRowCount(kSummaryRows);
    m_summaryTable->verticalHeader()->setVisible(false);
    m_summaryTable->setEditTriggers(QAbstractItemView::NoEditTriggers);   // 只读
    m_summaryTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_summaryTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_summaryTable->setFixedHeight(180);
    mainLayout->addWidget(m_summaryTable, 0);

    // 日志区（addWidget 第二个参数是拉伸因子，给它 1、其它给 0
    //           —— 这就是"只有日志区跟着窗口变高"的原因）
    m_logView = new QPlainTextEdit(central);
    m_logView->setReadOnly(true);
    mainLayout->addWidget(m_logView, 1);

    statusBar()->showMessage(QStringLiteral("就绪"));
    // 状态栏右侧用 addPermanentWidget 放引擎状态；
    // 左侧留给 showMessage —— M3 起那里显示实时数字（见 onUiTick）。
    statusBar()->addPermanentWidget(m_statusLabel = new QLabel(QStringLiteral("引擎：未启动"), this));

    // 左侧停靠窗口：暂时注释掉，等有真正的内容（脚本列表）时再启用。
    // 一个空的 QDockWidget 会在左侧留下一条空白栏，干扰你判断布局对不对。
    // QDockWidget *dock = new QDockWidget(QStringLiteral("文件夹"), this);
    // addDockWidget(Qt::LeftDockWidgetArea, dock);
}

void MainWindow::connectSignals()
{
    // --- 三个动作按钮 ---
    connect(m_startButton,  &QPushButton::clicked, this, &MainWindow::onStartClicked);
    connect(m_stopButton,   &QPushButton::clicked, this, &MainWindow::onStopClicked);
    connect(m_exportButton, &QPushButton::clicked, this, &MainWindow::onExportClicked);

    // --- 编排层的四个信号（界面与进程之间的全部接口）---
    connect(m_orchestrator, &TestOrchestrator::stateChanged, this, &MainWindow::onStateChanged);
    connect(m_orchestrator, &TestOrchestrator::logMessage,   this, &MainWindow::onLogMessage);
    connect(m_orchestrator, &TestOrchestrator::runFinished,  this, &MainWindow::onRunFinished);
    connect(m_orchestrator, &TestOrchestrator::failed,       this, &MainWindow::onFailed);

    // 注意 QTimer 的对象是 m_uiTimer。
    // 位置同样必须在 m_uiTimer 创建之后 —— 否则 connect 拿到 nullptr 只打印一行
    // 警告就静默跳过，状态栏永远不动（和 M2 那个 m_engine 的坑一模一样）。
    connect(m_uiTimer, &QTimer::timeout, this, &MainWindow::onUiTick);
}

void MainWindow::onStartClicked()
{
    // 从界面读参数。
    // 时长下拉的显示文本是给眼睛看的，真正传给 k6 的值用 addItem 的第二参数单独存 ——
    // 所以这里用 currentData() 而不是 currentText()。
    TestConfig config;
    config.targetUrl  = m_urlEdit->text().trimmed();
    config.vus        = m_vusSpin->value();
    config.duration   = m_durationCombo->currentData().toString();
    config.outputDir  = kReportsDir;
    // ★ 引擎路径交给 K6Engine 探测（Program Files / Program Files (x86) /
    //   chocolatey 三处），不写死某一个安装位置。
    config.k6Path     = K6Engine::detectK6Path();
    // ★ 示例脚本按 exe 位置反推：exe 在 out/build/bin/，示例在 <仓库根>/examples/。
    config.scriptPath = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../../examples/script_demo.js"));

    // ★ 参数校验（URL 非空 / 脚本存在 / 引擎可用）【不在这里】——
    //   那是编排层 preflight() 的职责。界面只管把"这一次的输入"交出去。
    //   preflight 失败时编排层发 failed(reason)，界面照常显示。
    m_orchestrator->startTest(config);
}

void MainWindow::appendLog(const QString &line)
{
    // ★ 这个函数会被频繁调用，但它【只能被低频调用】——
    //   它内部是 appendPlainText，一次调用 = 一次文档修改 + 重绘。
    //   每批数据都往这里灌，界面必卡。调用它的地方要先做好节流
    //   （节流在编排层的 onEngineOutput 里，见 TestOrchestrator.cpp）。
    m_logView->appendPlainText(line);
}

void MainWindow::setRunning(bool running)
{
    m_startButton->setEnabled(!running);    // 运行中不能再点开始
    m_stopButton->setEnabled(running);      // 没在跑就没什么可停
    m_exportButton->setEnabled(!running);   // 没跑完就别导出
    m_statusLabel->setText(running ? QStringLiteral("引擎：运行中")
                                   : QStringLiteral("引擎：未启动"));
}

void MainWindow::onStopClicked()
{
    // ★ 不要在这里调 terminate()。实测对 k6 完全无效：Windows 上 terminate() 靠给
    //   顶层窗口发 WM_CLOSE，而 k6 是无窗口控制台程序 —— 3 秒后进程还在跑。
    //   收尾动作（停定时器 / 填表）不写在这里，写在 onRunFinished 里 ——
    //   kill() 之后 finished 信号照常会发出来。
    m_orchestrator->stopTest();
}

void MainWindow::onExportClicked()   // 导出 Markdown 报告
{
    QString err;
    const QString path = m_orchestrator->exportReport(&err);
    if (path.isEmpty()) {
        appendLog(QStringLiteral("导出失败：%1")
                      .arg(err.isEmpty() ? QStringLiteral("未知原因") : err));
        return;
    }
    appendLog(QStringLiteral("已导出：%1").arg(path));
}

// ============================================================================
// 编排层回调
// ============================================================================

void MainWindow::onStateChanged(bool running)
{
    if (running) {
        // ★ 曲线要跟着新一轮一起清。
        //   ★ 不清会怎样：第二轮压测的曲线会从上一轮停下的地方接着往右画，
        //     两条测试的数据混在一张图上，X 轴还会从上一轮的秒数继续走。
        //   ★ 顺序：先 reset 图表、再 start 定时器。反过来的话，定时器可能
        //     在被清空之前先 tick 一次，往新图里插一个上一轮的脏点。
        m_chartView->reset();
        m_uiTimer->start();
        onUiTick();          // 立刻先刷一次，把上一轮的数字清掉，
                             // 否则第一秒里状态栏就挂着上次跑完的旧数字
    } else {
        m_uiTimer->stop();
    }
    setRunning(running);
}

void MainWindow::onLogMessage(const QString &line)
{
    appendLog(line);
}

void MainWindow::onRunFinished(const TestRunResult &result, const QString &summaryError)
{
    // --- 收尾 ---
    m_uiTimer->stop();
    onUiTick();          // 定格最后一帧（状态栏读的是已定格的 elapsedMs）
    setRunning(false);

    if (!summaryError.isEmpty())
        appendLog(QStringLiteral("（终值解析失败：%1 —— 已用实时数据兜底）").arg(summaryError));

    fillSummaryTable(result, summaryError);
    appendLog(QStringLiteral("结果保存在 %1").arg(kReportsDir));
}

void MainWindow::onFailed(const QString &reason)
{
    appendLog(reason);
    m_uiTimer->stop();
    setRunning(false);
}

// ============================================================================
// 汇总面板
// ============================================================================

void MainWindow::fillSummaryTable(const TestRunResult &r, const QString &summaryError)
{
    // 数据来源由「终值解析是否成功」决定，不由"文件在不在"决定。
    const QString source = (r.totalRequests > 0 && summaryError.isEmpty())
                               ? QStringLiteral("k6 汇总（summary.json）")
                               : QStringLiteral("实时统计（未经 k6 汇总）");

    // 显示用的小工具：耗时 0 一律显示 "—"，不显示 "0.00 ms"。
    const auto fmtMs = [](double v) -> QString {
        return v > 0.0 ? QStringLiteral("%1 ms").arg(v, 0, 'f', 2)
                       : QStringLiteral("—");
    };

    const QString requests = QString::number(r.totalRequests);
    const QString failures = QString::number(qint64(r.errorRate * double(r.totalRequests) + 0.5));
    const QString avgMs    = fmtMs(r.avgDurationMs);
    const QString p95Ms    = fmtMs(r.p95DurationMs);
    const QString p99Ms    = fmtMs(r.p99DurationMs);

    const auto put = [this](int row, const QString &name, const QString &value) {
        m_summaryTable->setItem(row, 0, new QTableWidgetItem(name));
        m_summaryTable->setItem(row, 1, new QTableWidgetItem(value));
    };

    put(kRowRequests, QStringLiteral("请求总数"), requests);
    put(kRowFailures, QStringLiteral("失败数"),   failures);
    put(kRowAvg,      QStringLiteral("平均值"),   avgMs);
    put(kRowP95,      QStringLiteral("p95"),      p95Ms);
    put(kRowP99,      QStringLiteral("p99"),      p99Ms);
    put(kRowSource,   QStringLiteral("数据来源"), source);
}

// ============================================================================
// 界面刷新
// ============================================================================

void MainWindow::onUiTick()
{
    MetricsAggregator *agg = m_orchestrator->metrics();

    // ① 先推进秒表 —— 漏了的话状态栏的秒数会永远停在 0.0。
    //    ★ 为什么时间要在这里推进、而不是在 feed() 里顺手更新？
    //      因为"数据到达"和"秒表推进"是两件独立的事。压测过程中完全可能
    //      有整整一秒没有任何样本（比如 VU 都在等响应），那一秒秒数也得继续走。
    //      所以：数据统计归 feed()，时间推进归这个每秒一拍的定时器。
    agg->refreshElapsed();

    // --- 把这一秒的增量喂给曲线 ---
    //   ★ 位置必须在 refreshElapsed() 【之后】—— takeWindow() 要用最新的
    //     elapsedMs 去算窗口秒数，早调用一秒就把分母算错了。
    //   ★ takeWindow() 是【取走】语义：调用一次就把水位推到当前位置。
    //     同一秒内调两次，第二次拿到的是空窗口（requestCount = 0）。
    //     所以"曲线每秒一个点"这条性质，是靠【调用频率】保证的，
    //     不是靠曲线自己数 —— 这行写在 onUiTick 里，它就自然是每秒一个点。
    //   ★ 曲线凭什么知道 X 轴画在哪？靠 WindowSample::epochMs（绝对时间戳），
    //     RealtimeChartView 内部拿第一个点当原点换算成相对秒。
    //     所以这里传的值不需要做任何时间换算。
    m_chartView->appendSample(agg->takeWindow());

    // ② 把聚合器的数字拼成一句话刷到状态栏
    const MetricsAggregator::Snapshot &s = agg->stats();
    statusBar()->showMessage(
        QStringLiteral("已采集 %1 点 | 请求 %2 次 | 失败 %3 | 已跑 %4s")
            .arg(s.points).arg(s.requests).arg(s.failedCount)
            .arg(s.elapsedMs / 1000.0, 0, 'f', 1));
    //   ★ 为什么用 showMessage 而不是改 m_statusLabel？
    //     状态栏是"左边临时消息区 + 右边常驻控件"的结构：
    //       · 左边 showMessage()          → 可被覆盖，适合"此刻正在发生什么"
    //       · 右边 addPermanentWidget()   → 永久驻留，适合"引擎状态"这种一直在的
    //     所以：左边刷实时数字，右边留给「引擎：运行中/未启动」（setRunning 在管）。
    //
    //   ★ 这个函数只有 3 行 —— 这是对的。界面上"跳动的数字"背后，
    //     靠的是把刷新频率从"每批数据"降到了"每秒一次"。
}

// ============================================================================
// 自测与验证
//
// 【M3 回归】数据管道随时可以单独验证（不用起界面）：
//   bash tools/probes/corpus_replay_probe_run.sh
//   期望：lines=2000  metricDecls=14  points=1986  requests=142  parseErrors=0
//   两种喂法（整块 / 每 7 字节）结果必须完全一致。
//
// 【M3 回归】界面联动：
//   bash tools/probes/m3_e2e_probe_run.sh normal   # 自然跑完
//   bash tools/probes/m3_e2e_probe_run.sh stop     # 第 4 秒点停止
//   验收过的数字：normal 请求 1483 == summary.json http_reqs.count 1483。
//
// 【M7 分层验收】界面里搜不到进程代码：
//   把 src/MainWindow.h / src/MainWindow.cpp 里的 #include <QProcess> 删掉，
//   能编译通过 = 界面真的不再依赖进程。
// 