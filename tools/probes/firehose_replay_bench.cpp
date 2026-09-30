// ============================================================================
// 火管回放基准探针 —— 两个问题一起回答
//
// 问题① 单线程用【现在的】MetricsAggregator 能吃下多少行/秒？
//   为什么要单独测：tools/probes/k6_firehose_bench.cpp 报的"最坏 42,867 行/s、
//   占墙钟 3.43%"是【探针自己读得多少】，它的回调里只数行、不解析 JSON ——
//   完全没覆盖 M3 才引入的 QJsonDocument::fromJson 逐行成本。
//   本探针测的是解析成本，且【必须分 Debug / Release 各测一次】：
//   两者差 20 倍以上，混在一起测会得出完全相反的结论。
//
// 问题② 如果只解析我们关心的 4 个指标（前缀白名单预筛），能快多少？
//   实测依据：k6 的 json 输出里每行都以 {"metric":" 固定开头，
//   且 14 个指标的行数完全相等（每请求 14 行）。关心的只有 4 个
//   → 71% 的行可以靠一次 memcmp 直接扔掉，连 JSON 解析器都不进。
//
// 用法：
//   firehose_replay_bench.exe <捕获文件> [块大小] [Release|Debug 仅影响日志文字]
//   bash tools/probes/firehose_replay_bench_run.sh <捕获文件> [块大小] [构建类型]
// ============================================================================

#include "MetricsAggregator.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// 候选优化：前缀白名单预筛
//
// 前提（实测，不是假设）：k6 的 json 输出里，数据行都是
//     {"metric":"<名字>","type":"Point","data":{...}}
// 即 "metric" 是第一个键、名字紧跟在固定偏移 11 处。指标声明行则是
//     {"type":"Metric",...}
// 以 "type" 开头 —— 所以这一条 memcmp 同时把声明行也筛掉了。
//
// 为什么这样能省下大头：QJsonDocument::fromJson 要对整行做词法+语法分析、
// 建一棵 QJsonObject 树、为每个键值分配内存（tags 里有 5~8 个键）。
// 而这里只需要"看 11 个字节 + 比一个短字符串"，是纯内存比较、零分配。
// ---------------------------------------------------------------------------
static const char *kWanted[] = {
    "http_reqs",          // M4：RPS 曲线的唯一数据源
    "http_req_failed",    // M4：失败数
    "vus",                // M5：VU 曲线
    "http_req_duration"   // M5：p95 / avg 延迟曲线的数据源
};
static const int kWantedCount = int(sizeof(kWanted) / sizeof(kWanted[0]));

static inline bool isWantedLine(const QByteArray &line)
{
    // {"metric":" === 11 字节
    static const char kPrefix[] = "{\"metric\":\"";
    constexpr int kPrefixLen = 11;
    if (line.size() < kPrefixLen + 4) return false;
    if (std::memcmp(line.constData(), kPrefix, kPrefixLen) != 0) return false;

    const char *p = line.constData() + kPrefixLen;
    for (int i = 0; i < kWantedCount; ++i) {
        const size_t n = std::strlen(kWanted[i]);
        // p[n] 必须是 '"' —— 否则 "http_req_" 这种前缀会误判
        if (std::memcmp(p, kWanted[i], n) == 0 && p[n] == '"') return true;
    }
    return false;
}

// 只走前缀白名单 + 轻量取 value 的候选实现（刻意【不】碰用户源码，
// 只为把"这条优化值多少"量出来；真要落地由用户自己写进 handleLine）
static void feedFiltered(const QByteArray &raw, int chunkSize, MetricsAggregator::Snapshot &out)
{
    MetricsAggregator::Snapshot s;
    QByteArray pending;
    auto handleOne = [&](const QByteArray &line) {
        if (line.isEmpty()) return;
        ++s.lines;
        if (!isWantedLine(line)) { ++s.metricDecls; return; }   // 借用 metricDecls 记"扔掉的行"

        // 走到这里才做真解析 —— 只解析留下的 4/14
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) { ++s.parseErrors; return; }
        const QJsonObject o = doc.object();
        if (o.value(QStringLiteral("type")).toString() != QStringLiteral("Point")) return;
        ++s.points;
        const QString metric = o.value(QStringLiteral("metric")).toString();
        const double value = o.value(QStringLiteral("data")).toObject()
                              .value(QStringLiteral("value")).toDouble();
        if (metric == QStringLiteral("http_reqs"))              s.requests += qint64(value);
        else if (metric == QStringLiteral("http_req_failed")) { if (value != 0.0) ++s.failedCount; }
        else if (metric == QStringLiteral("vus"))               s.vus = int(value);
    };

    if (chunkSize <= 0) {
        pending = raw;
        const qsizetype last = pending.lastIndexOf('\n');
        const QList<QByteArray> rows = pending.left(last + 1).split('\n');
        for (const QByteArray &r : rows) handleOne(r);
    } else {
        for (qsizetype i = 0; i < raw.size(); i += chunkSize) {
            pending.append(raw.mid(i, chunkSize));
            const qsizetype last = pending.lastIndexOf('\n');
            if (last < 0) continue;
            const QList<QByteArray> rows = pending.left(last + 1).split('\n');
            pending.remove(0, last + 1);
            for (const QByteArray &r : rows) handleOne(r);
        }
    }
    out = s;
}

int main(int argc, char **argv)
{
    const QString path = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QString();
    const int chunkSize = (argc > 2) ? std::atoi(argv[2]) : 32768;

    if (path.isEmpty()) {
        std::printf("用法: firehose_replay_bench <捕获文件> [块大小字节，0=整块]\n");
        return 2;
    }

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("RESULT=CANNOT_OPEN %s\n", qPrintable(path));
        return 2;
    }
    const QByteArray raw = f.readAll();
    f.close();

    std::printf("输入: %s\n", qPrintable(path));
    std::printf("大小=%.1f MB  换行符=%lld\n\n",
                raw.size() / 1024.0 / 1024.0, (long long)raw.count('\n'));

    const int sizes[] = { chunkSize > 0 ? chunkSize : 32768, 65536 };
    MetricsAggregator::Snapshot base;
    qint64 baseMs = 0;

    for (int pass = 0; pass < 2; ++pass) {
        for (int s : sizes) {
            if (pass == 1 && s != sizes[0]) continue;   // 第二遍只跑主块大小，省时间

            QElapsedTimer t;
            MetricsAggregator::Snapshot sn;
            t.start();
            if (pass == 0) {
                MetricsAggregator agg;
                agg.reset();
                agg.start();
                for (qsizetype i = 0; i < raw.size(); i += s) agg.feed(raw.mid(i, s));
                sn = agg.stats();
            } else {
                feedFiltered(raw, s, sn);
            }
            const qint64 ms = t.elapsed();
            const double sec = ms / 1000.0;
            const char *tag = (pass == 0) ? "现状(全量解析)" : "候选(4/14 预筛) ";

            std::printf("[块=%7d] %s 耗时 %9lld ms | %8.0f 行/s | %6.1f MB/s | "
                        "lines=%lld points=%lld req=%lld failed=%lld vus=%d\n",
                        s, tag, (long long)ms,
                        sec > 0 ? sn.lines / sec : 0.0,
                        sec > 0 ? raw.size() / 1024.0 / 1024.0 / sec : 0.0,
                        (long long)sn.lines, (long long)sn.points,
                        (long long)sn.requests, (long long)sn.failedCount, sn.vus);

            if (pass == 0 && s == sizes[0]) { base = sn; baseMs = ms; }
            if (pass == 1) {
                // ---- 正确性对账：4 个业务字段必须逐位相等 ----
                const bool ok = (sn.requests == base.requests)
                             && (sn.failedCount == base.failedCount)
                             && (sn.vus == base.vus);
                std::printf("            对账: req %s  failed %s  vus %s\n",
                            sn.requests == base.requests ? "PASS" : "FAIL",
                            sn.failedCount == base.failedCount ? "PASS" : "FAIL",
                            sn.vus == base.vus ? "PASS" : "FAIL");
                if (baseMs > 0 && ms > 0) {
                    std::printf("            ★ 提速 = %.2fx  (旧 %.0f 行/s → 新 %.0f 行/s)\n",
                                double(baseMs) / double(ms),
                                base.lines / (baseMs / 1000.0), sn.lines / (ms / 1000.0));
                }
                if (!ok) return 1;
            }
        }
        if (pass == 0) std::printf("\n");
    }

    return 0;
}
