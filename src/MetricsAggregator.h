#pragma once

// ============================================================================
// MetricsAggregator —— 把 k6 的原始样本流"数成数字"
//
// 在「喂进字节、数出数字」之上，多一个动作：
//     takeWindow() —— 每秒取走一个增量窗口，交给曲线
//
// 为什么这个类到现在【仍然不继承 QObject】：
//   原计划是"需要每秒广播一个窗口给图表，所以它得改成 QObject"。
//   真写起来发现不需要 —— MainWindow 本来就有个每秒一拍的 QTimer，
//   取数方（onUiTick）已经在正确的节拍上了，再插一层信号纯属多余。
//   所以改成【主动取】（takeWindow）而不是【被动收】（signal）。
//   ★ 结论：不要因为"将来可能需要信号"就提前继承 QObject。
//     什么时候真的需要了，什么时候再改 —— 现在改就是白付一次代价。
//
// ★★ 已验证过的五件事（全是实测结论，不是"据说"）★★
//
//   ① 管道边界 ≠ 行边界
//      readyRead 给到的 chunk 大概率在某一行中间断开。平均每次回调 18~21 行，
//      一条半行就能毁掉一条数据，而且频率低到极难复现。必须保留 m_pending。
//
//   ② 原始流里的 counter 是【增量 delta】，不是累计值
//      实测 http_reqs 的每个 Point 的 value 恒为 1（语料里 142 个点全是 1）。
//      所以必须 +=；直接赋值只会永远得到 1。
//      注意这与 REST API / summary.json 里的累计值语义【相反】。
//
//   ③ vus 是稀疏 gauge
//      它是"当前值"不是"累计量"，所以用【赋值】而不是累加。
//      且极稀疏（每秒才一条）：2,000 行语料里一条都没有，真实跑 10 秒才有 10 条。
//
//   ④ 流的末尾混着非 JSON 文本
//      k6 跑完后会在 stdout 尾部输出 THRESHOLDS / TOTAL RESULTS 人类可读摘要
//      （实测 23~30 行）。--quiet 关不掉它；--summary-mode disabled 能关掉，
//      但它会【连 --summary-export 一起干掉】。所以解析器必须对脏行宽容。
//
//   ⑤ QElapsedTimer::elapsed() 在 isValid() == false 时返回垃圾值
//      实测未 start / 已 invalidate 的情况下返回 -9223352315 ms。
//      所以读它之前必须先判 isValid()（见下面 refreshElapsed()）。
// ============================================================================

#include "types.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>

class MetricsAggregator
{
public:
    // 一眼能看懂的实时快照 —— 状态栏显示的就是它
    struct Snapshot {
        quint64 bytes       = 0;   // 累计收到的原始字节数
        quint64 lines       = 0;   // 收到的行数（含被跳过的 Metric 声明行）
        quint64 points      = 0;   // type == "Point" 的行数 —— 核心数字
        quint64 metricDecls = 0;   // type == "Metric" 的声明行数（是元信息，不是数据）
        quint64 parseErrors = 0;   // 解析失败的行数（尾部人类摘要会落进这里，属正常）
        qint64  requests    = 0;   // 累计请求数（http_reqs 的 delta 累加）
        qint64  failedCount = 0;   // 失败的请求数（http_req_failed 里 value != 0 的点数）
        int     vus         = 0;   // 最近一次 vus 采样值（gauge：赋值不是累加）
        qint64  elapsedMs   = 0;   // 从 start() 起算的毫秒数，由 refreshElapsed() 更新
    };

    void reset();                        // 每次点「开始压测」前调用：清空一切
    void start();                        // 开始计时
    void stop();                         // 停止计时（保留已累计的 elapsedMs）

    void feed(const QByteArray &chunk);  // ★ 核心：喂原始字节进来

    // --- 窗口取数 ---
    // 取走「自上次调用以来」的增量窗口 —— 每秒由 onUiTick() 调一次，喂给曲线。
    //
    // ★ 是【取走】语义，不是【查询】：调用一次就把水位推到当前位置。
    //   同一秒内调两次，第二次拿到的是空窗口（requestCount = 0，rps = 0）——
    //   这是刻意设计的，因为曲线的 X 轴是"秒"，一秒只能有一个点。
    // ★ 必须在 refreshElapsed() 【之后】调用：窗口秒数是用 elapsedMs 的差值算的，
    //   早调用一秒就把分母算错了。
    WindowSample takeWindow();

    // 状态栏每秒调一次。elapsedMs 不在 feed() 里更新，是为了让
    // "时间轴推进" 与 "数据到达" 解耦 —— 没有数据来的那一秒，秒数也要继续走。
    void refreshElapsed()
    {
        if (m_clock.isValid())          // ← 别删这个判断，见头注释 ⑤
            m_stats.elapsedMs = m_clock.elapsed();
    }

    const Snapshot &stats() const { return m_stats; }

    // 只给调试和单测看：当前还挂着多少"半行"
    qint64 pendingBytes() const { return m_pending.size(); }

private:
    void handleLine(const QByteArray &line);   // 处理一行（调用方保证不含 '\n'）

    Snapshot      m_stats;
    QByteArray    m_pending;    // ★ 半行残留，第一个必踩的坑
    QElapsedTimer m_clock;      // 计时器

    // --- 窗口水位（takeWindow 靠这几个"上次值"算差值）---
    qint64 m_lastWindowRequests  = 0;   // 上次 takeWindow() 时的 requests 水位
    qint64 m_lastWindowElapsedMs = 0;   // 上次 takeWindow() 时的 elapsedMs 水位
    double m_lastWindowFailedCount = 0;  // 上次 takeWindow() 时的 failedCount 水位
    QList<double> m_durationsMs;         // 每个点对应的耗时（毫秒）
};
