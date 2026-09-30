#pragma once

// ============================================================================
// BenchBoard 全局数据结构
//
// ★ 这个文件分两批成形，粒度是【由依赖决定的、不是由"什么时候用得上"决定的】：
//     WindowSample —— 一个 1 秒窗口的聚合结果，图表上的一个点
//     TestConfig / CheckResult / TestRunResult —— 汇总面板与报告的数据源
//   后三个必须【一起】加：TestRunResult 的字段里嵌着 TestConfig 和
//   QVector<CheckResult> —— 字段类型不完整就编译不过。
//
// 约定：
//   - 本文件只放「纯数据」，不放逻辑、不引用 Qt Widgets
//   - 数值单位统一：时间用 ms，时间戳用 Unix 毫秒（qint64）
//   - 术语统一：RPS / VU / p95 / 窗口(window)
//     禁止出现 QPS、并发数、百分位等混用词
// ============================================================================

#include <QString>
#include <QDateTime>
#include <QVector>

enum class MetricKind {
    HttpReqs,         // counter：累计请求数
    HttpReqDuration,  // trend  ：单次请求耗时（ms）
    HttpReqFailed,    // rate   ：失败请求（value 为 1 表示失败）
    Vus               // gauge  ：当前虚拟用户数（稀疏，见 MetricsAggregator）
};

// 一个 1 秒窗口的聚合结果 —— 图表上的一个点
struct WindowSample {
    qint64 epochMs       = 0;    // 窗口结束时刻（Unix 毫秒）；曲线的 X 轴就是由它换算出来的

    // ---- 图表用：实时窗口内可直接算出的四个字段 ----
    qint64 requestCount  = 0;    // 本窗口内完成的请求数（原始值，便于和 k6 对账）
    double rps           = 0.0;  // 请求速率 = requestCount / 窗口秒数
    int    vus           = 0;    // 本窗口 VU 数（无样本时由上一窗口前向填充）
    bool   hasVusSample  = false;// false 表示 vus 是填充值而非实测值

    // ---- 汇总用：窗口内的耗时分布统计（无样本时恒为 0.0）----
    double avgDurationMs = 0.0;  // 本窗口耗时均值
    double p95DurationMs = 0.0;  // 本窗口 p95 耗时
    double errorRate     = 0.0;  // 本窗口失败占比 0.0 ~ 1.0
};

// 一次压测的输入配置
struct TestConfig {
    QString targetUrl;
    QString scriptPath;                                   // k6 脚本绝对路径
    int     vus = 10;
    QString duration = QStringLiteral("30s");           // k6 语法：30s / 1m / 90s
    QString k6Path;                                       // 引擎可执行文件
    QString outputDir;                                    // 报告输出目录

    bool isValid() const { return !scriptPath.isEmpty() && vus > 0 && !duration.isEmpty(); }
};

// 单个 k6 check（断言）的结果
struct CheckResult {
    QString name;
    qint64  passes = 0;
    qint64  fails = 0;
};

// 一次压测的最终结果 —— 汇总面板与 Markdown 报告的唯一数据源
struct TestRunResult {
    TestConfig            config;
    QDateTime             startedAt;
    qint64                totalRequests = 0;
    double                rps = 0.0;   // 全程平均 RPS
    double                minDurationMs = 0.0;
    double                avgDurationMs = 0.0;
    double                medDurationMs = 0.0;
    double                maxDurationMs = 0.0;
    double                p90DurationMs = 0.0;
    double                p95DurationMs = 0.0;
    double                p99DurationMs = 0.0;
    double                errorRate = 0.0;   // 0.0 ~ 1.0
    QVector<CheckResult>  checks;
    QString               engineVersion;

    bool isValid() const { return totalRequests > 0; }
};