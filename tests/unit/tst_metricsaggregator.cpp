// ============================================================================
// MetricsAggregator —— 数据管道能不能算对
//
// 为什么这层值得单独测：管道错了，界面照样"看着在跳"。肉眼盯状态栏永远
// 发现不了"点数恒为 0"或"rps 恒等于累计值"这类错 —— 只有拿固定语料对账
// 才能把它翻红。语料是 2000 行、期望值精确，1 秒内跑完。
//
// 核心判据是【切行粒度不影响结果】：
//   管道边界 ≠ 行边界（readyRead 给到的 chunk 大概率断在某一行中间）。
//   所以同一份语料按 1/3/7/64/4096 字节切碎喂进去，结果必须和整块喂逐字段一致。
//   这里用数据驱动让断言只写一遍，六种粒度各跑一次 —— 比"手工比两种模式"更严。
//
// 来源：tools/probes/corpus_replay_probe.cpp（M3 验收），路径改走 TestPaths。
// ============================================================================

#include <QtTest>

#include "Corpus.h"
#include "MetricsAggregator.h"

#include <QList>

class TestMetricsAggregator : public QObject
{
    Q_OBJECT

private slots:
    void replayMatchesExpectation_data();
    void replayMatchesExpectation();

    void allChunkingsAgree();

    void dirtyLinesAreCountedNotFatal();
    void partialLineIsHeld();
    void partialLineIsJoined();
    void resetClearsEverything();
    void secondRunDoesNotInheritFirstRun();
    void stopFreezesElapsed();

private:
    // 把同一份语料按指定粒度喂进聚合器，返回快照。
    // chunkSize <= 0 表示整块喂。
    static MetricsAggregator::Snapshot replay(const QByteArray &raw, int chunkSize);
};

MetricsAggregator::Snapshot TestMetricsAggregator::replay(const QByteArray &raw, int chunkSize)
{
    MetricsAggregator agg;
    agg.reset();
    agg.start();
    if (chunkSize <= 0) {
        agg.feed(raw);
    } else {
        for (int i = 0; i < raw.size(); i += chunkSize)
            agg.feed(raw.mid(i, chunkSize));
    }
    return agg.stats();
}

// ---------------------------------------------------------------------------
// 六种喂法，同一组期望值
// ---------------------------------------------------------------------------
void TestMetricsAggregator::replayMatchesExpectation_data()
{
    QTest::addColumn<int>("chunkSize");

    QTest::newRow("整块喂")        << 0;
    QTest::newRow("每 1 字节")     << 1;      // 最狠：每一批都断在行中间
    QTest::newRow("每 3 字节")     << 3;
    QTest::newRow("每 7 字节")     << 7;      // 原探针的粒度，跨行边界不整除
    QTest::newRow("每 64 字节")    << 64;
    QTest::newRow("每 4096 字节")  << 4096;   // 一批十几行，回到"正常"情况
}

void TestMetricsAggregator::replayMatchesExpectation()
{
    QFETCH(int, chunkSize);

    const QByteArray raw = Corpus::readStream();
    QVERIFY2(!raw.isEmpty(),
             qPrintable(QStringLiteral("读不到语料：%1").arg(Corpus::streamFile())));

    const Corpus::Expectation want;
    const MetricsAggregator::Snapshot got = replay(raw, chunkSize);

    QCOMPARE(qint64(got.bytes),       qint64(raw.size()));
    QCOMPARE(qint64(got.lines),       want.lines);
    QCOMPARE(qint64(got.metricDecls), want.metricDecls);
    QCOMPARE(qint64(got.points),      want.points);   // ★ 漏写 ++points 就恒为 0，且不报错
    QCOMPARE(got.requests,            want.requests); // ★ 当成累计值处理就恒为 1
    QCOMPARE(got.failedCount,         want.failedCount);
    QCOMPARE(qint64(got.parseErrors), want.parseErrors);
}

void TestMetricsAggregator::allChunkingsAgree()
{
    const QByteArray raw = Corpus::readStream();
    QVERIFY2(!raw.isEmpty(), "读不到语料");

    // 整块喂是基准
    const MetricsAggregator::Snapshot ref = replay(raw, 0);

    const QList<int> sizes = {1, 3, 7, 13, 64, 1024, 4096, 65536};
    for (int size : sizes) {
        const MetricsAggregator::Snapshot got = replay(raw, size);

        const auto mismatch = [size](const char *field) {
            return QStringLiteral("chunkSize=%1：%2 与整块喂不一致")
                       .arg(size).arg(QLatin1StringView(field)).toUtf8();
        };

        QVERIFY2(got.bytes       == ref.bytes,       mismatch("bytes"));
        QVERIFY2(got.lines       == ref.lines,       mismatch("lines"));
        QVERIFY2(got.points      == ref.points,      mismatch("points"));
        QVERIFY2(got.metricDecls == ref.metricDecls, mismatch("metricDecls"));
        QVERIFY2(got.parseErrors == ref.parseErrors, mismatch("parseErrors"));
        QVERIFY2(got.requests    == ref.requests,    mismatch("requests"));
        QVERIFY2(got.failedCount == ref.failedCount, mismatch("failedCount"));
    }
}

// ---------------------------------------------------------------------------
// 边界
// ---------------------------------------------------------------------------

// 脏行只计数，绝不 throw / 中断。
// 这不是假想情况：k6 跑完会在 stdout 尾部输出 THRESHOLDS / TOTAL RESULTS
// 人类可读摘要（实测 23~30 行），它们必然解析失败。
void TestMetricsAggregator::dirtyLinesAreCountedNotFatal()
{
    MetricsAggregator agg;
    agg.reset();

    agg.feed(QByteArrayLiteral("not json at all\n"));
    agg.feed(QByteArrayLiteral("\n"));                       // 空行：不进 lines
    agg.feed(QStringLiteral("THRESHOLDS\nhttp_req_duration\n  ✓ 'p(95)<500'\n").toUtf8());

    QCOMPARE(qint64(agg.stats().lines),       qint64(4));
    QCOMPARE(qint64(agg.stats().parseErrors), qint64(4));
    QCOMPARE(qint64(agg.stats().points),      qint64(0));
}

// 半行必须挂起，不能当完整行解析
void TestMetricsAggregator::partialLineIsHeld()
{
    MetricsAggregator agg;
    agg.reset();
    agg.feed(QByteArrayLiteral("{\"type\":\"Point\",\"met"));

    QCOMPARE(qint64(agg.stats().lines), qint64(0));
    QCOMPARE(agg.pendingBytes(),        qint64(20));
}

// 下半行送到后补成一条完整行，且残留清空
void TestMetricsAggregator::partialLineIsJoined()
{
    MetricsAggregator agg;
    agg.reset();
    agg.feed(QByteArrayLiteral("{\"type\":\"Point\",\"met"));
    agg.feed(QByteArrayLiteral("ric\":\"vus\",\"data\":{\"value\":7}}\n"));

    QCOMPARE(qint64(agg.stats().lines),  qint64(1));
    QCOMPARE(qint64(agg.stats().points), qint64(1));
    QCOMPARE(agg.stats().vus,            7);      // gauge：赋值不是累加
    QCOMPARE(agg.pendingBytes(),         qint64(0));
}

// reset() 必须连残留半行一起清干净
void TestMetricsAggregator::resetClearsEverything()
{
    MetricsAggregator agg;
    agg.reset();
    agg.feed(QByteArrayLiteral("{\"type\":\"Point\",\"met"));
    agg.reset();

    QCOMPARE(qint64(agg.stats().lines), qint64(0));
    QCOMPARE(agg.stats().vus,           0);
    QCOMPARE(agg.pendingBytes(),        qint64(0));
}

// ★ 「新增了状态，就回来 reset 里清它」—— 这条规则在本项目里已经被踩过两次
//   （M4 漏清水位、M5 又漏一次）。症状是：忘了不会报错，只在【第二轮】压测发作。
//
//   这里直接验它的后果：第一轮跑完后窗口水位停在 142，第二轮不清水位的话，
//   第一个窗口会算出「第二轮累计(0) − 第一轮总量(142)」= −142 的差值，
//   曲线开头炸出一根向下的尖峰。
void TestMetricsAggregator::secondRunDoesNotInheritFirstRun()
{
    MetricsAggregator agg;

    // --- 第一轮 ---
    agg.reset();
    agg.start();
    agg.feed(Corpus::readStream());
    const WindowSample first = agg.takeWindow();
    QCOMPARE(first.requestCount, qint64(Corpus::Expectation{}.requests));  // 水位推到这里

    // --- 第二轮：一切从头 ---
    agg.reset();
    agg.start();
    const WindowSample second = agg.takeWindow();

    QCOMPARE(second.requestCount, qint64(0));   // 不是 -142，也不是 142
    QCOMPARE(second.rps,          0.0);
    QCOMPARE(second.errorRate,    0.0);
}

// stop() 之后 elapsedMs 必须定格，不能被清零；
// 且未 start 时读 elapsedMs 不能拿到 QElapsedTimer 的垃圾值。
void TestMetricsAggregator::stopFreezesElapsed()
{
    MetricsAggregator agg;
    agg.reset();
    agg.start();
    agg.stop();
    agg.refreshElapsed();

    QVERIFY2(agg.stats().elapsedMs >= 0,
             qPrintable(QStringLiteral("elapsedMs = %1（负数 = 读了未 start 的 QElapsedTimer）")
                            .arg(agg.stats().elapsedMs)));
    QVERIFY2(agg.stats().elapsedMs < 50,
             qPrintable(QStringLiteral("elapsedMs = %1").arg(agg.stats().elapsedMs)));
}

QTEST_GUILESS_MAIN(TestMetricsAggregator)
#include "tst_metricsaggregator.moc"
