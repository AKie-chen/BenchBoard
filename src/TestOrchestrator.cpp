#include "TestOrchestrator.h"

#include "K6Engine.h"
#include "MetricsAggregator.h"
#include "ReportWriter.h"
#include "SummaryParser.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

// ============================================================================
// 编排层实现（M7-4）
//
// ★ 本文件里【没有】QProcess，也不该有。验收判据别写成 `grep -c QProcess == 0`——
//   注释里提到 `QProcess::` 也会被数进去。正解是「删掉 #include <QProcess> 后仍能编译」。
//
// ★ 它持有的两个子对象，托管方式【不同】—— 逐个走「三步判断法」：
//     m_engine  → K6Engine（QObject 子类）→ new + parent = this → 走 QObject 摘牌机制
//     m_metrics → MetricsAggregator（【不】继承 QObject，拿不到 parent）
//                 → 只能自己 delete，所以析构里那一行不能删
// ============================================================================

TestOrchestrator::TestOrchestrator(QObject *parent)
    : QObject(parent)
{
    m_engine  = new K6Engine(this);        // parent = this → 由 QObject 父子链负责回收
    m_metrics = new MetricsAggregator();   // 不是 QObject → 自己 delete（见析构）

    // 数据通路：引擎的字节 → 聚合器。本类唯一"接线"的地方。
    connect(m_engine, &LoadEngine::outputChunk, this, &TestOrchestrator::onEngineOutput);
    connect(m_engine, &LoadEngine::finished,    this, &TestOrchestrator::onEngineFinished);
    connect(m_engine, &LoadEngine::startFailed, this, &TestOrchestrator::onEngineStartFailed);

    // 引擎有什么话要说（stderr 诊断 / 健康度告警），原样抬到界面日志区。
    connect(m_engine, &LoadEngine::logOutput,     this, &TestOrchestrator::logMessage);
    connect(m_engine, &LoadEngine::healthWarning, this, &TestOrchestrator::logMessage);
}

TestOrchestrator::~TestOrchestrator()
{
    // m_engine 挂在 QObject 父子链上，这里不用管。
    // m_metrics 不继承 QObject、没有 parent —— 不写这一行就是内存泄漏。
    delete m_metrics;
}

LoadEngine *TestOrchestrator::engine() const
{
    return m_engine;
}

MetricsAggregator *TestOrchestrator::metrics() const
{
    return m_metrics;
}

// 启动前自检：引擎可用 + URL 合法 + 脚本存在。
// 返回空字符串表示通过；否则返回可直接展示给用户的中文原因。
QString TestOrchestrator::preflight(const TestConfig &config) const
{
    // ★ 这里是 const 函数 —— 只做只读校验，别顺手改状态。
    //   顺序：先看"引擎在不在"，再看"参数对不对"。
    if (!m_engine->isAvailable())
        return QStringLiteral("未找到 k6 可执行文件，请检查安装路径。");

    if (config.targetUrl.isEmpty()) {
        // ★ 为什么必须有：URL 通过 -e BASE_URL= 传给脚本，空串传过去 k6 报的错是
        //   "unsupported protocol scheme"，排查半天才发现是自己没填。三行代码，省半小时。
        return QStringLiteral("请先填写目标 URL");
    }
    if (!config.isValid())
        return QStringLiteral("脚本不存在或者参数不合法。");
    if (!QFileInfo::exists(config.scriptPath))
        return QStringLiteral("找不到脚本文件：%1").arg(config.scriptPath);

    return QString();
}

// 引擎可用性诊断文本，用于启动时在日志区显示
QString TestOrchestrator::detectEngine() const
{
    const QString path = K6Engine::detectK6Path();
    if (path.isEmpty())
        return QStringLiteral("未找到 k6 可执行文件，请检查安装路径。");

    const QString version = K6Engine::queryVersion(path);
    if (version.isEmpty())
        return QStringLiteral("k6 版本查询失败，请检查 k6 是否正常工作。");

    return QStringLiteral("k6 版本：%1").arg(version);
}

void TestOrchestrator::startTest(const TestConfig &config)
{
    // ★ 先记 config + 复位本轮标志，再做自检。
    //   这样即使自检没过，用户点「导出报告」也能拿到"这一次的输入"。
    //   ★ 三态标志必须在这里重置 —— 否则上一轮的值会残留成这一轮的结果
    //     （同族：M5 那笔"陈旧 summary"的账）。
    m_config           = config;
    m_startedAt        = QDateTime::currentDateTime();
    m_lastResult       = TestRunResult();
    m_wallMs           = 0;
    m_userStopped      = false;
    m_lastCleanExit    = false;
    m_lastSummaryPath.clear();
    m_firstChunkLogged = false;
    m_lastLoggedLines  = 0;

    const QString reason = preflight(config);
    if (!reason.isEmpty()) {
        emit failed(reason);
        return;
    }

    // ★ k6 不会替你把目录建出来 —— 目录不在，--summary-export 就【静默不产出】。
    //   这个目录原本靠 reports/.gitkeep 撑着，换台机器或删库重建就不在了。
    QDir().mkpath(config.outputDir);

    // --- 开新一轮统计 ---
    // ★ 顺序有讲究：reset 在前、start 在后。反过来的话 m_clock 刚起表就被清掉，
    //   时间轴会永远停在 0。
    m_metrics->reset();
    m_metrics->start();

    m_running = true;
    emit stateChanged(true);
    emit logMessage(QStringLiteral("引擎已启动，目标：%1").arg(config.targetUrl));

    m_engine->start(config);   // 异步：立刻返回，结果经信号回来
}

void TestOrchestrator::stopTest()
{
    if (!m_engine->isRunning()) return;
    // ★ 三态里的"是不是用户让我停的"—— 这件事只有编排层知道，引擎不可能知道。
    m_userStopped = true;
    emit logMessage(QStringLiteral("用户请求停止（强杀 k6；terminate() 对无窗口控制台程序无效）"));
    m_engine->stop();
}

bool TestOrchestrator::isRunning() const
{
    return m_running;
}

const TestRunResult &TestOrchestrator::lastResult() const
{
    // ★ m_lastResult 是【值成员】不是指针 —— 它是 M6「唯一取值入口」
    //   （buildRunResult）的终点形态。
    return m_lastResult;
}

// 导出 Markdown 报告，返回文件路径；失败返回空串并写入 errorOut
QString TestOrchestrator::exportReport(QString *errorOut)
{
    if (m_config.outputDir.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("还没有压测配置，无法导出报告。");
        return QString();
    }

    // ★ 表格与报告读的是同一个 m_lastResult —— 这就是"唯一取值入口"。
    QString err;
    const QString path = ReportWriter::writeMarkdown(m_lastResult, m_config.outputDir, &err);
    if (path.isEmpty()) {
        if (errorOut) *errorOut = err.isEmpty() ? QStringLiteral("未知原因") : err;
        return QString();
    }
    return path;
}

// 引擎的原始字节 → 聚合器；顺带两处节流日志
void TestOrchestrator::onEngineOutput(const QByteArray &chunk)
{
    m_metrics->feed(chunk);

    // --- 节流日志（原在 MainWindow::onEngineStdout）---
    //   实测最坏 42867 行/s、readyRead 每秒回调约 2400 次；把这些全灌进
    //   QPlainTextEdit → 重绘跟不上 → 界面假死。所以 99% 的调用什么都不做。
    const MetricsAggregator::Snapshot &s = m_metrics->stats();
    if (!m_firstChunkLogged) {
        emit logMessage(QString::fromUtf8(chunk.left(2 * 1024)));   // 首包：看清数据长什么样
        m_firstChunkLogged = true;
    } else if (s.lines - m_lastLoggedLines >= 10000) {
        emit logMessage(QStringLiteral("已接收 %1 行 / %2 MB")
                            .arg(s.lines).arg(s.bytes / 1024.0 / 1024.0, 0, 'f', 1));
        m_lastLoggedLines = s.lines;
    }
}

// 非正常退出时的兜底：拿实时累计值填结果，不编耗时分布
void TestOrchestrator::fillFromRealtime()
{
    const MetricsAggregator::Snapshot &s = m_metrics->stats();
    m_lastResult.totalRequests = s.requests ? s.requests : 0;
    // 除零守卫：requests == 0 时别算出 nan / inf
    m_lastResult.errorRate = s.requests ? double(s.failedCount) / double(s.requests) : 0.0;
    // ★ 实时侧的 Snapshot 里【没有】耗时分布（要全程 avg/p95 得另存全部样本），
    //   所以这三个写 0 —— ReportWriter 会把 0 渲染成 "—"，不编一个数字出来。
    m_lastResult.avgDurationMs = m_lastResult.p95DurationMs = m_lastResult.p99DurationMs = 0.0;
}

void TestOrchestrator::onEngineFinished(int exitCode, bool cleanExit, const QString &summaryJsonPath)
{
    m_running         = false;
    m_lastCleanExit   = cleanExit;
    m_lastSummaryPath = summaryJsonPath;   // ★ 只能从这一参存下来（接口上没有 summary 路径）
    m_wallMs          = m_metrics->stats().elapsedMs;

    m_lastResult.startedAt = m_startedAt;
    m_lastResult.config    = m_config;

    QString summaryError;

    if (cleanExit) {
        emit logMessage(QStringLiteral("引擎正常结束（退出码 %1）").arg(exitCode));
        // 只有【正常退出】才允许读 summary.json
        if (!SummaryParser::parseFile(summaryJsonPath, &m_lastResult, &summaryError)) {
            // 解析失败：实时数据仍然有效 → 兜底，别把空结果当成功
            fillFromRealtime();
            if (summaryError.isEmpty()) summaryError = QStringLiteral("未知原因");
            emit logMessage(QStringLiteral("summary.json 解析失败：%1（已回退到实时统计）")
                                .arg(summaryError));
        } else {
            emit logMessage(QStringLiteral("已读取 k6 汇总：%1").arg(summaryJsonPath));
        }
    } else {
        // ★ 判据是【退出状态】不是"文件在不在"：强杀之后那个文件多半是【陈旧】的、不是不存在。
        emit logMessage(QStringLiteral("引擎被强制结束（退出码 %1，此值无语义，别拿它做判断）")
                            .arg(exitCode));
        fillFromRealtime();
        const QString why = m_userStopped ? QStringLiteral("用户停止") : QStringLiteral("异常退出");
        emit logMessage(QStringLiteral("本次为「%1」—— 不读 summary.json"
                                       "（它此刻要么不存在，要么是上一轮的陈旧文件）").arg(why));
    }

    emit runFinished(m_lastResult, summaryError);
}

void TestOrchestrator::onEngineStartFailed(const QString &reason)
{
    m_running = false;
    emit failed(reason);
}
