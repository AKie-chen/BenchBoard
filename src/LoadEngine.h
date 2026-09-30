#pragma once

// ============================================================================
// 压测引擎抽象接口
//
// 为什么要这一层抽象：
//   本项目的核心难点是「界面与数据通道」，不该和「用哪个压测工具」绑死。
//   将来加 vegeta / 自研 C++ 引擎 = 新增一个实现类 + 在 TestOrchestrator
//   里改一行，UI 层零改动。
//
// 实现者须知：
//   ① 绝不能用 QProcess::start("cmd /c ...") 这类拼字符串的方式启动子进程
//      —— URL 与脚本路径来自用户输入，会被命令注入。
//      必须 setProgram() + setArguments() 走参数列表。
//   ② 引擎的运行期告警（启动超时、缓冲积压、进程异常退出）通过 healthWarning 上报，
//      不要静默吞掉。
//      （"解析失败"目前由聚合器计数 —— 它不继承 QObject、没有信号，这是已知要还的债。）
// ============================================================================

#include "types.h"

#include <QByteArray>
#include <QObject>
#include <QString>

class LoadEngine : public QObject
{
    Q_OBJECT

public:
    explicit LoadEngine(QObject *parent = nullptr) : QObject(parent) {}
    ~LoadEngine() override = default;

    // 引擎显示名，例如 "k6"
    virtual QString name() const = 0;

    // 探测引擎是否可用（可执行文件存在、能跑出版本号）。
    // 返回 false 时把可读原因写入 errorOut，供界面显示。
    virtual bool isAvailable(QString *errorOut = nullptr) const = 0;

    // 启动压测。这是一个异步操作：立即返回，结果通过信号通知。
    // 启动失败通过 startFailed() 上报，不要抛异常、不要阻塞等待。
    virtual void start(const TestConfig &config) = 0;

    // 请求停止。
    // ⚠ 实测（k6 v2.2.0 / Windows）：QProcess::terminate() 对无窗口的控制台程序
    //   【完全无效】——它靠给顶层窗口发 WM_CLOSE，k6 没有窗口，3 秒后进程仍在跑。
    //   所以实现里直接 kill()，不要做"优雅终止"的幻想，也不要让用户等。
    //   代价是强杀后引擎不会产出汇总文件，上层必须能用实时数据兜底。
    virtual void stop() = 0;

    virtual bool isRunning() const = 0;

signals:
    // ---- 实时数据通道 ----
    //
    // ★ 这里发的是【字节】而不是结构化样本，是有意的：
    //   一条 "void rawSample(MetricKind, qint64, double, bool)" 式的信号看起来更"分层"，
    //   但它把解析结果当成了引擎的契约 —— 换 vegeta 时字段对不上，等于白抽象。
    //   现在的写法是「聚合器主动吃字节」，解析只住在一个地方。
    //
    //   代价（已知并接受）：引擎抽象降级成了"字节管道"——
    //   这笔债记为【已知遗留】：换引擎时聚合器仍要改。
    //
    // 引擎吐出的原始输出字节。k6 是 stdout 上的 JSON 行流（每条一行、带 '\n'）。
    // MetricsAggregator::feed() 直接吃它 —— 拆行 / 半行残留 / JSON 解析都在那边。
    //
    // 注意：实测 4.8 万行/秒时，发信号的开销可以忽略（同线程 AutoConnection
    // 等同于一次普通函数调用）。真正的坑是「每次回调都去刷新 UI」，不是发信号本身。
    void outputChunk(const QByteArray &chunk);

    // ---- 过程日志 ----
    // 引擎的 stderr / 诊断信息，逐行交给界面日志区。
    // 必须消费 stderr：实测极端场景 k6 在 8 秒内写了 5.9 MB stderr，
    // 不读会导致管道缓冲区写满，子进程被阻塞住。
    void logOutput(const QString &line);

    // ---- 生命周期 ----
    void started();

    // ★ 为什么除了退出码还要传一个 cleanExit：
    //   QProcess::ExitStatus 是【唯一】能判"正常退出还是被强杀"的东西，
    //   只传退出码等于把这个判据截断在实现类里。上层至少要用它做两件事：
    //     ① 决定能不能读 summary.json。★ 强杀之后那个文件多半是【陈旧】的、
    //        不是"不存在"——所以判据是【退出状态】而不是【文件在不在】。
    //     ② 区分「用户主动停」和「意外崩」（后者要上层自己补，引擎不可能知道）。
    //
    //   ★ 退出码顶不了这个位置：实测被强杀时它是 62097（无语义数字）；
    //     NormalExit 下 0=全通过、99=thresholds 被突破 —— 只在【已知是正常退出】
    //     的前提下才有含义。
    //
    //   ★ 为什么传 bool 而不是 QProcess::ExitStatus：
    //     传后者，任何实现这个接口的类、以及 TestOrchestrator 就都得
    //     #include <QProcess> —— 直接破掉"编排层不认识 QProcess"这条分层判据。
    //
    //     cleanExit == true   进程自己退出的：exitCode 有语义，summaryJsonPath 可信
    //     cleanExit == false  被 kill() / 崩掉的：exitCode 无意义，
    //                         summaryJsonPath 可能指向上一轮留下的陈旧文件
    void finished(int exitCode, bool cleanExit, const QString &summaryJsonPath);

    void startFailed(const QString &reason);

    // ---- 健康度 ----
    // 解析失败行数超阈值、或缓冲积压时上报，界面应显示告警而不是假装没事。
    void healthWarning(const QString &message);
};
