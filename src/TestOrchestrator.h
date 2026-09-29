#pragma once

// ============================================================================
// 编排层：把「引擎 → 聚合 → 终值 → 报告」串起来
//
// 分层铁律：
//   UI 只认识 TestOrchestrator，不认识 QProcess / K6Engine / 解析器 / 聚合器。
//   这样换引擎、换解析方式都不用动界面代码。
//
// 一次压测的完整时序：
//    startTest()
//      → 记 config / 复位三态标志 → preflight 自检 → engine->start()
//      → 引擎发 outputChunk(原始字节) → 本类转手喂 m_metrics->feed()
//      → UI 自己每秒一拍，向 metrics() 取 takeWindow() 画点（是【拉取】，不是被动收信号）
//      → 引擎 finished(退出码, 是否正常退出, summary 路径)
//      → cleanExit ? SummaryParser 解析 : 实时数据兜底 → 填 lastResult
//      → runFinished(result, summaryError) → UI 显示汇总
//    exportReport()
//      → ReportWriter 落盘 → 返回路径
//
// ★ 契约修正记录（两条，2026-09-27）：
//   ① 原文写的是「引擎逐条 rawSample → 聚合器每秒发 windowCompleted → UI 画点」。
//      那两个东西【都不存在】：`rawSample` 在 M7 已删、`windowCompleted` M3 起就没有。
//      现在的真相是【拉取】不是【推送】—— 自述见 `MetricsAggregator.h:9-15`。
//   ② `windowReady` 信号**已删**：与①同因（M3 定的就是拉取，`metrics()` 这个 getter
//      才是活的）。留一个没人接的"另一条路"，下一个人会以为它通 —— 这就是 `scanLine`
//      那一簇的教训。
//   完整记录见 `_target/README.md` 的「契约修正记录」。
// ============================================================================

#include "types.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class LoadEngine;
class MetricsAggregator;

class TestOrchestrator : public QObject
{
    Q_OBJECT

public:
    explicit TestOrchestrator(QObject *parent = nullptr);
    ~TestOrchestrator() override;

    // 引擎接入点：本期返回内部持有的 K6Engine。
    // 将来支持多引擎时，这里改成可注入，UI 依然零改动。
    LoadEngine *engine() const;

    MetricsAggregator *metrics() const;

    // 启动前自检：引擎可用 + URL 合法 + 脚本存在。
    // 返回空字符串表示通过；否则返回可直接展示给用户的中文原因。
    QString preflight(const TestConfig &config) const;

    // 引擎可用性诊断文本，用于启动时在日志区显示
    QString detectEngine() const;

    void startTest(const TestConfig &config);
    void stopTest();
    bool isRunning() const;

    const TestRunResult &lastResult() const;

    // 导出 Markdown 报告，返回文件路径；失败返回空串并写入 errorOut
    QString exportReport(QString *errorOut = nullptr);

signals:
    void stateChanged(bool running);

    // 过程日志（首包样本 / 进度行 / stderr / 状态提示）统一从这一条上去。
    // ★ 界面不再自己读聚合器的快照来打日志 —— 那是编排层的活。
    void logMessage(const QString &line);

    // summaryError 非空表示终值解析失败（实时数据仍然有效，报告要用实时数据兜底）
    void runFinished(const TestRunResult &result, const QString &summaryError);
    void failed(const QString &reason);

private slots:
    // 引擎的原始字节 → 聚合器。顺带做两处节流日志（首包 / 每 10000 行一行进度）。
    // （原在 `MainWindow::onEngineStdout`，M7 随数据通路一起下沉。）
    void onEngineOutput(const QByteArray &chunk);

    // ★ 契约修正（2026-09-27）：这里原本只有 (exitCode, summaryJsonPath) ——
    //   退出判据在 `emit` 的那一刻就被截断了，编排层拿不到（详见 README 契约修正记录）。
    //
    //   分工（★ 谁都不该去猜对方的活）：
    //     cleanExit     由【引擎】给 —— 引擎只知道"进程是怎么结束的"
    //     m_userStopped 由【本类】给 —— "是不是用户让我停的" 是编排层的知识，
    //                   引擎不可能知道
    //   → 三态：cleanExit / !cleanExit && m_userStopped / !cleanExit && !m_userStopped
    void onEngineFinished(int exitCode, bool cleanExit, const QString &summaryJsonPath);
    void onEngineStartFailed(const QString &reason);

private:
    // 非正常退出（被 kill / 崩掉）时的兜底：拿实时累计值填结果，不编耗时分布。
    void fillFromRealtime();

    LoadEngine        *m_engine  = nullptr;
    MetricsAggregator *m_metrics = nullptr;
    TestConfig         m_config;
    TestRunResult      m_lastResult;
    QDateTime          m_startedAt;
    qint64             m_wallMs  = 0;
    bool               m_running = false;

    // ★ 三态判定用（契约修正 2026-09-27 新增）
    //   startTest() 里【必须重置这三个】—— 否则上一轮的值会残留成这一轮的结果
    //   （同族：M5 那笔"陈旧 summary"的账）。
    bool               m_userStopped   = false;   // stopTest() 置 true
                                                  // ★ 已读：决定日志说"用户停止"还是"异常退出"
    bool               m_lastCleanExit = false;   // onEngineFinished 存 cleanExit
    QString            m_lastSummaryPath;         // 同上：路径只能从 finished() 的第三参存下来
                                                  // （LoadEngine 抽象接口上【没有】summary 路径）
    // ⚠️ 上面两个成员目前【只写不读】。契约原意是"导出报告时读"（让报告能标注
    //    "本次数据来自实时统计，不是 k6 汇总"），但 M7 收尾时 exportReport() 还没用上
    //    它们 —— 接线 = 改 M6 报告的输出内容，会破坏 M7 自己的验收前提
    //    「重构不改变行为（m6_report_probe 37/37 一行不变）」。
    //    → 记为【已知遗留】，留给 M8 或后续独立小补丁（见 `_target/README.md`）。
    //    ⚠️ 别照着旧注释推测"已经有地方在读它"—— 实测全仓零读取点。

    // 日志节流状态 —— 属于编排层，因为产生日志的地方在这里。
    bool    m_firstChunkLogged = false;   // 首包样本是否已打印
    quint64 m_lastLoggedLines  = 0;       // 上次打进度行时的行数水位
};
