#include "MetricsAggregator.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <algorithm>
#include <cmath>     // std::ceil（算 p95 下标）；不显式加只是碰巧被别的头带入，不稳
#include <numeric>

// ============================================================================
// MetricsAggregator —— 从 k6 的 JSON 流里数出数字
//
//   三个要点（也是唯一需要动脑的三处）：
//     ① 管道边界 ≠ 行边界 —— m_pending 半行残留
//     ② 同一条流里有三种累加语义 —— += / ++ / =（见 handleLine 结尾）
//     ③ 解析器必须容忍脏行 —— 尾部 23~30 行人类摘要必然解析失败
//
//   ★ 踩过的坑：漏写 `++m_stats.points;` → 请求数照涨、点数恒为 0，且【不报任何错】。
//     这类"漏一行但不报错"的错误，是靠 A3 断言（结束时点数 > 0）抓出来的；
//     肉眼盯着状态栏只会觉得"数字在跳，应该没问题"。
// ============================================================================

void MetricsAggregator::reset()
{
    // 三行清零。m_clock 必须 invalidate 而不是放着不管：
    // 两次压测之间 onUiTick 可能先跑一次，refreshElapsed() 里那个 isValid()
    // 判断就是为这种情况准备的 —— 没有它 elapsed() 会返回 -9223352315。
    m_stats = Snapshot{};
    m_pending.clear();
    m_clock.invalidate();

    // ★ 清掉窗口水位。
    //   不清的话，第二轮压测的第一个窗口会算出"第二轮累计 − 第一轮总量"，
    //   曲线开头会莫名其妙炸出一根巨大的尖峰。
    //
    //   ★ 这条是通用规则，值得记住：**新增了状态，就回来 reset 里清它。**
    //     忘了不会报错，只会在"第二次跑"的时候发作 —— 最难查的那类 bug。
    m_lastWindowElapsedMs = 0;
    m_lastWindowRequests = 0;
    m_lastWindowFailedCount = 0;
    m_durationsMs.clear();
}

void MetricsAggregator::start()
{
    m_clock.start();
}

void MetricsAggregator::stop()
{
    // 注意【不要】在这里把 m_stats.elapsedMs 清零 —— 压测结束后状态栏还要
    // 显示"跑了多久"。invalidate() 只是让 refreshElapsed() 不再更新它，
    // 已经记下的值会定格在那里。
    m_clock.invalidate();
}

void MetricsAggregator::feed(const QByteArray &chunk)
{
    m_stats.bytes += quint64(chunk.size());

    // ★ 核心：先把新数据拼到【残留】后面，而不是直接解析 chunk。
    m_pending.append(chunk);

    // 找最后一个换行符 —— 它前面才是完整行，后面留回 m_pending。
    const qsizetype lastNewline = m_pending.lastIndexOf('\n');
    if(lastNewline < 0) return;              // 连一行都没凑齐，等下一批
    const QByteArray complete = m_pending.left(lastNewline + 1);   // 含尾部 \n
    m_pending.remove(0, lastNewline + 1);     // 剩下的就是新的半行残留

    const QList<QByteArray> rows = complete.split('\n');
    for (const QByteArray& row : rows) {
        if (row.isEmpty()) continue;          // 尾部 \n 会切出一个空元素，跳过
        handleLine(row);
    }

    // 边界（M7 会回来看这一条）：如果一整批数据里一个 '\n' 都没有（比如某一行
    // 特别长），上面会 return，数据全留在 m_pending 里等下一批 —— 这是对的，
    // 但也意味着 m_pending 理论上能无限长。
}

void MetricsAggregator::handleLine(const QByteArray &line)
{
    if(line.isEmpty()) return;
    ++m_stats.lines;

    // k6 的一行就是一个独立 JSON 对象，QJsonDocument 就够。
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        ++m_stats.parseErrors;
        return;
    }
    const QJsonObject obj = doc.object();

    // ★ 解析失败只计数、直接返回，【绝不】throw 或当成致命错误。
    //   流尾部一定混着人类可读摘要（THRESHOLDS / TOTAL RESULTS），实测 23~30 行，
    //   它们必然解析失败 —— 这是正常现象，不是 bug。
    //
    // type 只有两种取值：
    //   "Metric" —— 元信息（声明这个指标是什么类型），不是数据，跳过
    //   "Point"  —— 真正的样本，只关心它
    const QString type = obj.value(QStringLiteral("type")).toString();
    if ( type == QStringLiteral("Metric")) { ++m_stats.metricDecls; return; }
    if ( type != QStringLiteral("Point"))  { return; }
    ++m_stats.points;   // ★ 漏了这一行 → 请求数照涨、点数恒为 0（M3 验收 A3 就是这么 FAIL 的）

    // 为什么要专门数 metricDecls：声明行数 == 出现的指标个数（去重后相等，可自校验）。
    //   · 语料 2,000 行 → 14 个声明
    //   · 真实 10 秒跑 → 16 个声明（多出 vus / vus_max；它们是每秒才发一次的
    //     稀疏 gauge，2,000 行的短样本里还没轮到发）
    //   这个数字对不上就说明切行或分类逻辑出问题了 —— 比"看着像对的"可靠得多。
    const QString metric = obj.value(QStringLiteral("metric")).toString();
    const QJsonObject data = obj.value(QStringLiteral("data")).toObject();
    const double value = data.value(QStringLiteral("value")).toDouble();

    // ★ 第二个知识点：同一条流里三种累加语义，靠指标类型区分。
    //     · counter（http_reqs）在【原始流】里发的是增量：每个请求发一个 value=1 的点
    //     · rate（http_req_failed）每个请求发一个 0 或 1
    //     · gauge（vus）发的是当前值：每次采样告诉你"此刻有 8 个 VU"
    //   而 summary.json 里 counter 是【累计值】—— 两处语义相反。
    //   不知道这条，报告里就会出现"总请求数 = 1"。
    if ( metric == QStringLiteral("http_reqs")) {
        m_stats.requests += qint64(value);        // 累加！因为是 delta
    } else if ( metric == QStringLiteral("http_req_failed")) {
        if (value != 0.0) ++m_stats.failedCount;  // 每个请求一个点，值 0 或 1
    } else if (metric == QStringLiteral("vus")) {
        m_stats.vus = int(value);                 // 赋值！因为是 gauge
    } else if (metric == QStringLiteral("http_req_duration")) {
        m_durationsMs.append(value);
    }
    // 其余 13 个指标（http_req_blocked / data_sent / iterations …）不管。
    // 不写 else 分支就是"忽略"，这是有意的 —— 全解会白白多花约 3 倍解析时间。
}

// ============================================================================
// 把「累计值」变成「本秒增量」
// ============================================================================

WindowSample MetricsAggregator::takeWindow()
{
    // ★ 这个函数只有十行，但它是"曲线凭什么每秒一个点"的全部答案。
    //
    //   ② 取差值 —— 关键在于 m_stats 里存的是【累计值】，曲线要的是【增量】
    WindowSample w;
    w.epochMs = QDateTime::currentMSecsSinceEpoch();
    w.requestCount = m_stats.requests - m_lastWindowRequests;
    const qint64 dMs = m_stats.elapsedMs - m_lastWindowElapsedMs;
    //   ③ 算 RPS。分母兜个 1e-3 防 0 除 —— 第一次调用时 dMs 可能正好是 0
    w.rps = double(w.requestCount) / qMax(1e-3, double(dMs) / 1000.0);
    //   ⑤ vus 直接抄当前值：它本来就是 gauge（最近一次采样），天然就是"填充值"，
    //      所以曲线上的 VU 不需要额外写前向填充逻辑
    w.vus = m_stats.vus;
    w.hasVusSample = (m_stats.vus > 0);
    //   ④ 推进水位 —— ★ 必须放在最后一行。前面任何一步 return 掉都不能走到这里，
    //      否则那部分数据就被"吃掉"了，永远补不回来
    if (!m_durationsMs.isEmpty()) {
        QList<double> temp(m_durationsMs.begin(), m_durationsMs.end());
        std::sort(temp.begin(), temp.end());
        
        const double sum = std::accumulate(temp.begin(), temp.end(), 0.0);
        w.avgDurationMs = sum / temp.size();

        size_t n = temp.size();
        // P95位置公式： idx = ceil(n * 0.95) - 1 （0下标）
        size_t idx = static_cast<size_t>(std::ceil(n * 0.95)) - 1;
        w.p95DurationMs = temp[idx];
    }
    w.errorRate = w.requestCount > 0 ? double(m_stats.failedCount - m_lastWindowFailedCount) / double(w.requestCount) : 0.0;

    m_lastWindowRequests = m_stats.requests;
    m_lastWindowElapsedMs = m_stats.elapsedMs;
    m_lastWindowFailedCount = m_stats.failedCount;

    // ★ M5：算完就清 —— avg / p95 是【本窗口】的统计，不是全程累计。
    //   不清的话曲线会越跑越平（累积平均），数字看着正常，语义已经错了。
    m_durationsMs.clear();
    //   ★ ① 别忘了 reset() 里那几行清零 —— "新增了状态，就回来 reset 里清它"
    //     （这条规则已经在 M4 漏水位、M5 加水位两次踩过）。
    //     漏了不会报错，只会在【第二轮】压测发作，是最难查的那类 bug。
    //
    //   ★ avgDurationMs / p95DurationMs / errorRate 从 M5 起真正填上了：
    //     它们要的是"本窗口内的 trend 样本分布"，所以靠 m_durationsMs 攒缓冲、
    //     算完在上面清空。M4 只画 RPS 时用不上它们。
    //
    //   ★ 为什么时间用 QDateTime 取绝对时间，而不是用 m_clock？
    //     · m_clock 是相对计时器，两次压测之间会 invalidate，不适合当 X 轴基准
    //     · 曲线的 X 轴 = (epochMs - t0) / 1000，需要一个【绝对】时间戳才换算得出来
    //
    //   写完可以自问一句：如果这一秒一个请求都没有，w.rps 是多少？曲线会怎样？
    //   （答案：0。曲线会掉到 X 轴上 —— 这才是对的，RPS 为 0 就该显示 0。）
    //
    //   ★ 踩过的坑：上面四步全算对了，最后却写成了 `return WindowSample{};` ——
    //     返回一个默认构造的空对象，w 里算好的东西【全部丢掉】。
    //     症状：图表有标题有坐标轴，但【一条线都没有】。因为 rps / epochMs 全是 0，
    //     曲线所有点重合在原点 (0,0)，QLineSeries 只有一个点，画不出线段。
    //     ★ 这跟 M3 那个"漏写 ++m_stats.points"是同一族：**不报错**。
    //       `/W4` 也不报 —— w 被赋值过，C4189（已初始化未引用）不触发。
    //       只能靠 m4_window_probe 的守恒断言（ΣrequestCount == 语料总请求数）抓。
    return w;
}

// ============================================================================
// 自测方法
//
// 【语料回放】不用等界面接好，1 秒内出结果：
//   bash tools/probes/corpus_replay_probe_run.sh
//     模式① 整块喂   —— 一次 feed(全部 2000 行)
//     模式② 每 7 字节喂一次 —— 强迫边界断在行中间，逼出半行残留路径
//   两者必须结果完全一致，并和期望值对账：
//     lines = 2000   metricDecls = 14   points = 1986
//     requests = 142   failedCount = 0   parseErrors = 0
//
// 【端到端】管道对了再验界面：
//   bash tools/probes/m3_e2e_probe_run.sh normal   # 自然跑完
//   bash tools/probes/m3_e2e_probe_run.sh stop     # 第 4 秒点停止
// ============================================================================
