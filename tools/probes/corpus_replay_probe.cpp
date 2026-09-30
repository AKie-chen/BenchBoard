// ============================================================================
// 语料回放探针 —— 不碰界面、不碰 k6，直接回放已存好的真实流给 MetricsAggregator
//
// 为什么值得单独做这么一个探针（M3 的验收纪律）：
//   数据管道能不能算对，和界面显不显示是两件事。管道错了，界面照样"看着在跳"。
//   语料是固定的 2000 行，期望值精确 —— 这种"总数能对账"的验证，
//   比肉眼盯着状态栏可靠得多，而且 1 秒内跑完。
//
// 回放分两种模式，都要过（这是本关最容易漏的一点）：
//   ① 整块喂   —— 一次 feed(全部 2000 行)
//   ② 切碎喂   —— 每 7 字节喂一次，逼出"半行残留"路径
//   两者结果必须【完全一致】。不一致 = m_pending 的拼接/切除写错了。
//
// 跑法：bash tools/probes/corpus_replay_probe_run.sh
// ============================================================================

#include "MetricsAggregator.h"

#include <QByteArray>
#include <QFile>
#include <cstdio>

static int g_fail = 0;

static void check(const char *name, qint64 got, qint64 want)
{
    const bool ok = (got == want);
    if (!ok) ++g_fail;
    std::printf("  %-14s got=%-8lld want=%-8lld  %s\n",
                name, (long long)got, (long long)want, ok ? "PASS" : "FAIL");
}

// 把同一份语料按不同粒度喂进去，返回快照
static MetricsAggregator::Snapshot replay(const QByteArray &raw, int chunkSize)
{
    MetricsAggregator agg;
    agg.reset();
    agg.start();
    if (chunkSize <= 0) {
        agg.feed(raw);                      // 整块
    } else {
        for (int i = 0; i < raw.size(); i += chunkSize) {
            agg.feed(raw.mid(i, chunkSize)); // 切碎
        }
    }
    // 尾部若剩下半行（语料以 \n 结尾，正常应该剩 0），也一并报告
    return agg.stats();
}

int main(int argc, char **argv)
{
    const QString path = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                    : QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_stream_sample_2k.jsonl");

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("RESULT=CANNOT_OPEN %s\n", qPrintable(path));
        return 2;
    }
    const QByteArray raw = f.readAll();
    f.close();

    const int newlines = raw.count('\n');
    std::printf("语料: %s\n", qPrintable(path));
    std::printf("字节=%lld  换行符=%d\n\n", (long long)raw.size(), newlines);

    // ---- 模式① 整块喂 ----
    std::printf("[模式① 整块喂]\n");
    const MetricsAggregator::Snapshot whole = replay(raw, 0);
    check("lines",       qint64(whole.lines),       2000);
    check("metricDecls", qint64(whole.metricDecls), 14);
    check("points",      qint64(whole.points),      1986);
    check("requests",    whole.requests,            142);
    check("failedCount", whole.failedCount,         0);
    check("parseErrors", qint64(whole.parseErrors), 0);
    check("bytes",       qint64(whole.bytes),       raw.size());

    // ---- 模式② 切碎喂（7 字节一批，边界几乎必然断在行中间）----
    std::printf("\n[模式② 每 7 字节喂一次 —— 逼出半行残留路径]\n");
    const MetricsAggregator::Snapshot frag = replay(raw, 7);
    check("lines",       qint64(frag.lines),        2000);
    check("metricDecls", qint64(frag.metricDecls),  14);
    check("points",      qint64(frag.points),       1986);
    check("requests",    frag.requests,             142);
    check("failedCount", frag.failedCount,          0);
    check("parseErrors", qint64(frag.parseErrors),  0);

    // ---- 两种模式必须逐字段一致 ----
    std::printf("\n[模式① vs 模式② 逐字段一致]\n");
    const bool same = (whole.bytes == frag.bytes) && (whole.lines == frag.lines)
                   && (whole.points == frag.points) && (whole.metricDecls == frag.metricDecls)
                   && (whole.parseErrors == frag.parseErrors) && (whole.requests == frag.requests)
                   && (whole.failedCount == frag.failedCount);
    if (!same) ++g_fail;
    std::printf("  %s\n", same ? "PASS 切行粒度不影响结果" : "FAIL 粒度不同结果就不同 → m_pending 有 bug");

    // ---- 边界：脏行 / 空输入 / 半行挂起 ----
    std::printf("\n[边界]\n");
    {
        MetricsAggregator agg;
        agg.reset();
        // ① 谁都不该因为脏行崩掉或中断
        agg.feed(QByteArray("not json at all\n"));
        agg.feed(QByteArray("\n"));
        agg.feed(QByteArray("THRESHOLDS\nhttp_req_duration\n  ✓ 'p(95)<500'\n"));
        const bool dirtyOk = (agg.stats().parseErrors == 4) && (agg.stats().lines == 4);
        if (!dirtyOk) ++g_fail;
        std::printf("  %-14s lines=%lld parseErrors=%lld  %s\n", "脏行 3 条",
                    (long long)agg.stats().lines, (long long)agg.stats().parseErrors,
                    dirtyOk ? "PASS" : "FAIL");

        // ② 半行必须挂起，不能当完整行解析
        MetricsAggregator a2;
        a2.reset();
        a2.feed(QByteArray("{\"type\":\"Point\",\"met"));
        const bool held = (a2.stats().lines == 0) && (a2.pendingBytes() == 20);
        if (!held) ++g_fail;
        std::printf("  %-14s lines=%lld pending=%lld  %s\n", "半行挂起",
                    (long long)a2.stats().lines, (long long)a2.pendingBytes(),
                    held ? "PASS" : "FAIL");

        // ③ 下半行送到后应该补成一条完整行
        a2.feed(QByteArray("ric\":\"vus\",\"data\":{\"value\":7}}\n"));
        const bool joined = (a2.stats().lines == 1) && (a2.stats().points == 1)
                          && (a2.stats().vus == 7) && (a2.pendingBytes() == 0);
        if (!joined) ++g_fail;
        std::printf("  %-14s lines=%lld points=%lld vus=%d pending=%lld  %s\n", "半行接上",
                    (long long)a2.stats().lines, (long long)a2.stats().points, a2.stats().vus,
                    (long long)a2.pendingBytes(), joined ? "PASS" : "FAIL");

        // ④ reset 必须清干净（含残留半行）
        a2.reset();
        const bool cleared = (a2.stats().lines == 0) && (a2.stats().vus == 0) && (a2.pendingBytes() == 0);
        if (!cleared) ++g_fail;
        std::printf("  %-14s %s\n", "reset()", cleared ? "PASS" : "FAIL");

        // ⑤ 停止后 elapsedMs 必须定住（不许被清零）——stop() 之后再 refresh 不该改它
        MetricsAggregator a3;
        a3.reset();
        a3.start();
        a3.stop();
        a3.refreshElapsed();
        const bool frozen = (a3.stats().elapsedMs >= 0) && (a3.stats().elapsedMs < 50);
        if (!frozen) ++g_fail;
        std::printf("  %-14s elapsedMs=%lld  %s\n", "stop() 定格",
                    (long long)a3.stats().elapsedMs, frozen ? "PASS" : "FAIL");
    }

    std::printf("\nRESULT=%s\n", g_fail == 0 ? "ALL_PASS" : "HAS_FAIL");
    return g_fail == 0 ? 0 : 1;
}
