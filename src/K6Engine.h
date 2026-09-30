#pragma once

// ============================================================================
// k6 引擎实现（外部进程）
//
// k6 有三条数据通道，本类用了两条：
//   ① 火管  -o json=-            → stdout，实时样本。必须异步 + 批量读。
//   ② 终值  --summary-export     → 结束时落盘，路径经 finished() 传出
//                                    （★ 连带 cleanExit = 进程是否自己退出的，见下）。
//   ③ REST  --address /v1/metrics → 本期不用；它只有【累计】聚合值、
//                                    实测不含 p99、也没有 tags，做不了实时曲线。
//
// 实测到的 k6 v2.2.0 行为（和很多博客写的不一样，别再按老文档写）：
//   - `-o json=-` 才会写 stdout；写成 `-o json=stdout` 会被当成文件名，
//     然后生成一个叫 stdout 的 58 MB 文件。
//   - summary.json 里的 metric 【没有 type 字段】，不能靠 type 判断指标类型。
//   - metric 里可能混有 thresholds 这类非数值对象，遍历字段时必须判类型。
//   - 默认汇总不含 p99，必须显式传 --summary-trend-stats。
//   - --vus / --duration 命令行参数【覆盖】脚本里的 options.vus / options.duration。
//     想让界面上的输入框生效，就必须把这两个参数传下去。
//   - GET /v1/status 返回 502，不要拿它做存活探测。
//   - QProcess::terminate() 对 k6 无效（无窗口的控制台程序），只能 kill()。
//
// ★ 分层：本类只负责「把 stdout 字节原样发出去」（outputChunk）。
//   拆行 / 半行残留 / JSON 解析全部住在 MetricsAggregator。
//
//   为什么不在引擎里顺手解析：那样"引擎"与"解析"会纠缠在一起 ——
//   换引擎就得重写解析，改解析就得动引擎。曾经有一版设计把
//   scanLine() / m_pending / m_parsedLines 放在这里，结果三个成员全部失去消费者，
//   只剩一个被调用却什么都不做的空实现，最后整体删掉。
//
//   "把解析上移回引擎" 记为【已知遗留】—— 见 LoadEngine.h 里那笔同源的债。
// ============================================================================

#include "LoadEngine.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcess>
#include <QString>
#include <QTimer>

class K6Engine : public LoadEngine
{
    Q_OBJECT

public:
    explicit K6Engine(QObject *parent = nullptr);
    ~K6Engine() override;

    QString name() const override;
    bool    isAvailable(QString *errorOut = nullptr) const override;
    void    start(const TestConfig &config) override;
    void    stop() override;
    bool    isRunning() const override;

    // 探测常见安装位置下的 k6，返回可执行文件绝对路径；未找到返回空串
    static QString detectK6Path();

    // 读取引擎版本号（用于写入报告的环境信息），失败返回空串
    static QString queryVersion(const QString &k6Path);

    const QString &lastSummaryPath() const;

    // 组装 k6 命令行参数（纯函数）。
    // ★ 公开是【刻意】的：验收条款要求它"有单测、不用起进程"，private 会让这件事
    //   做不到 —— 把它放在 private 却注明"供单测使用"，是自相矛盾的。
    //   它不碰任何实例状态（连 summary 路径都靠 summaryPathFor(config) 推导），
    //   暴露出去不会给调用方留下"改坏内部状态"的机会。
    static QStringList buildArguments(const TestConfig &config);

private slots:
    void onReadyReadStdout();
    void onReadyReadStderr();
    // ★ cleanExit 的唯一来源是 status == QProcess::NormalExit。
    //   emit finished(exitCode, /*cleanExit=*/ status == QProcess::NormalExit, m_summaryPath);
    //   —— 这条判据【必须在走到上层之前就带上】，否则上层只能拿 exitCode 去猜（猜不出来）。
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);
    void onStopTimeout();

private:
    QProcess      m_process;
    QString       m_k6Path;
    QString       m_summaryPath;
    QElapsedTimer m_wallClock;        // 启动到停止的计时器
    QTimer        m_stopTimer;        // stop() 后的强杀兜底
    bool          m_stopping    = false;
};
