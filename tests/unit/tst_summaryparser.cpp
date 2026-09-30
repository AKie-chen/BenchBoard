// ============================================================================
// SummaryParser —— k6 summary.json 的解析契约
//
// 三个实测踩过的层级/类型陷阱（取错不会报错，只会静默失败）：
//   ① avg / p(95) 这些【不在 metrics 顶层】，在 metrics["http_req_duration"] 里
//   ② 该子对象里可能混进非数值键 "thresholds": {...}
//      —— 语料单测全绿 ≠ 真实数据正确（真实产出就有这个键）
//   ③ root_group.checks 是【对象】不是数组：
//      调 toArray() 得到空数组 → 循环 0 次 → checks 永远为空，零报错
//
// 还有一条语义契约：缺字段 ≠ 解析失败。
//   p(99) 缺失时必须"解析成功但该字段保持 0"，由显示层画成 "—"。
//   把两者混为一谈，就会让整次解析作废。
//
// 来源：tools/probes/summary_parser_probe.cpp。
// ============================================================================

#include <QtTest>

#include "Corpus.h"
#include "SummaryParser.h"

#include <QFile>

namespace {

// JSON 里的数值经过一次 字符串→double 的往返，用 1e-3 容差比，
// 免得因为第 16 位有效数字的表示差异把测试弄红。
// （不用 QCOMPARE：qFuzzyCompare 在一侧为 0 时的行为不适合这里的用例。）
bool close(double got, double want) { return qAbs(got - want) < 1e-3; }

QString dbl(double v) { return QString::number(v, 'f', 6); }

}  // namespace

class TestSummaryParser : public QObject
{
    Q_OBJECT

private slots:
    void corpusParses_data();
    void corpusParses();

    void cleanSampleFullFieldSet();

    void thresholdsSubObjectIsNotANumber();
    void missingP99KeepsDefaultInsteadOfFailing();
    void checksAreObjectsNotArrays();

    void invalidJsonReportsReason();
    void missingMetricsReportsReason();
    void missingFileReportsReason();

    void nullResultIsRejected();
};

// ---------------------------------------------------------------------------
// 三个语料：文件 → 期望值
// ---------------------------------------------------------------------------
void TestSummaryParser::corpusParses_data()
{
    QTest::addColumn<QString>("path");
    QTest::addColumn<qint64>("totalRequests");
    QTest::addColumn<double>("avg");
    QTest::addColumn<double>("p95");
    QTest::addColumn<double>("p99");
    QTest::addColumn<double>("errorRate");
    QTest::addColumn<int>("checkCount");
    QTest::addColumn<qint64>("check0Passes");

    QTest::newRow("干净的汇总样本")
        << Corpus::summaryClean()
        << qint64(4375) << 3.0926225599999984 << 4.0206 << 4.828576000000002
        << 0.0 << 1 << qint64(4375);

    QTest::newRow("带 thresholds 子对象（真实产出形态）")
        << Corpus::summaryThreshold()
        << qint64(1112) << 8.223578327338119 << 10.451295 << 12.128175000000002
        << 0.0 << 1 << qint64(1112);

    QTest::newRow("缺 p(99)")
        << Corpus::summaryMissingP99()
        << qint64(100) << 1.5 << 0.0 << 0.0
        << 0.0 << 0 << qint64(0);
}

void TestSummaryParser::corpusParses()
{
    QFETCH(QString, path);
    QFETCH(qint64, totalRequests);
    QFETCH(double, avg);
    QFETCH(double, p95);
    QFETCH(double, p99);
    QFETCH(double, errorRate);
    QFETCH(int, checkCount);
    QFETCH(qint64, check0Passes);

    TestRunResult r;
    QString err;
    QVERIFY2(SummaryParser::parseFile(path, &r, &err),
             qPrintable(QStringLiteral("解析失败：%1 —— %2").arg(path, err)));

    QCOMPARE(r.totalRequests, totalRequests);
    QVERIFY2(close(r.avgDurationMs, avg), qPrintable(dbl(r.avgDurationMs) + " vs " + dbl(avg)));
    QVERIFY2(close(r.p95DurationMs, p95), qPrintable(dbl(r.p95DurationMs) + " vs " + dbl(p95)));
    QVERIFY2(close(r.p99DurationMs, p99), qPrintable(dbl(r.p99DurationMs) + " vs " + dbl(p99)));
    QVERIFY2(close(r.errorRate, errorRate),
             qPrintable(dbl(r.errorRate) + " vs " + dbl(errorRate)));
    QCOMPARE(r.checks.size(), checkCount);
    if (checkCount > 0)
        QCOMPARE(r.checks.first().passes, check0Passes);
}

// 干净样本的其余字段（这些只在一个样本上有意义，不塞进上表）
void TestSummaryParser::cleanSampleFullFieldSet()
{
    TestRunResult r;
    QString err;
    QVERIFY2(SummaryParser::parseFile(Corpus::summaryClean(), &r, &err), qPrintable(err));

    QVERIFY2(close(r.rps, 874.7668921185882), qPrintable(dbl(r.rps)));
    QVERIFY2(close(r.minDurationMs, 1.0373),  qPrintable(dbl(r.minDurationMs)));
    QVERIFY2(close(r.medDurationMs, 3.0654),  qPrintable(dbl(r.medDurationMs)));
    QVERIFY2(close(r.maxDurationMs, 10.2638), qPrintable(dbl(r.maxDurationMs)));
    QVERIFY2(close(r.p90DurationMs, 3.74232), qPrintable(dbl(r.p90DurationMs)));

    QCOMPARE(r.checks.size(), 1);
    QCOMPARE(r.checks.first().name, QStringLiteral("status is 200"));
    QCOMPARE(r.checks.first().fails, qint64(0));
    QVERIFY(err.isEmpty());
}

// ★ http_req_duration 里混着 "thresholds": { "p(95)<500": false } 这个子对象。
//   takeDouble() 用 isDouble() 挡掉它 —— 不挡的话，取到的是子对象，
//   整个解析静默失败（takeDouble 恒返回 false，字段全是默认值）。
void TestSummaryParser::thresholdsSubObjectIsNotANumber()
{
    const QByteArray raw = [this] {
        QFile f(Corpus::summaryThreshold());
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();
    QVERIFY2(!raw.isEmpty(), "读不到语料");
    QVERIFY2(raw.contains("\"thresholds\""), "语料里应当有 thresholds 子对象，否则这条用例失效");

    TestRunResult r;
    QString err;
    QVERIFY2(SummaryParser::parseJson(raw, &r, &err), qPrintable(err));

    // 子对象被正确跳过：该在的字段都在
    QVERIFY2(close(r.p95DurationMs, 10.451295), qPrintable(dbl(r.p95DurationMs)));
    QVERIFY2(close(r.avgDurationMs, 8.223578327338119), qPrintable(dbl(r.avgDurationMs)));
    QCOMPARE(r.totalRequests, qint64(1112));
}

// ★ 缺字段 ≠ 解析失败：
//   必须"解析成功 + 该字段保持默认 0"，而不是整体作废。
//   （0 由显示层画成 "—" —— 失败与缺字段是两件事。）
void TestSummaryParser::missingP99KeepsDefaultInsteadOfFailing()
{
    TestRunResult r;
    QString err;
    QVERIFY2(SummaryParser::parseFile(Corpus::summaryMissingP99(), &r, &err),
             qPrintable(QStringLiteral("缺 p(99) 不应导致解析失败：%1").arg(err)));

    QCOMPARE(r.p99DurationMs, 0.0);
    QCOMPARE(r.minDurationMs, 0.0);
    QCOMPARE(r.maxDurationMs, 0.0);
    QCOMPARE(r.totalRequests, qint64(100));   // 有的字段照常读出来
    QCOMPARE(r.checks.size(), 0);             // root_group.checks 是空对象
}

// ★ root_group.checks 是【对象】不是数组。调 toArray() 会得到空数组、
//   循环 0 次、checks 永远为空 —— 而且零报错。
void TestSummaryParser::checksAreObjectsNotArrays()
{
    TestRunResult r;
    QString err;
    QVERIFY2(SummaryParser::parseFile(Corpus::summaryClean(), &r, &err), qPrintable(err));

    QCOMPARE(r.checks.size(), 1);
    QCOMPARE(r.checks.first().name, QStringLiteral("status is 200"));
    QCOMPARE(r.checks.first().passes, qint64(4375));   // 对象里真正有值，才证明是遍历不是取空
}

// ---------------------------------------------------------------------------
// 失败路径：必须【失败 + 给出可读原因】两条都成立。
// 曾经写成 else 分支，结果是"解析成功反而报错、解析失败反而没原因"。
// ---------------------------------------------------------------------------
void TestSummaryParser::invalidJsonReportsReason()
{
    TestRunResult r;
    QString err;
    QVERIFY2(!SummaryParser::parseJson(QByteArrayLiteral("{ not json"), &r, &err),
             "非法 JSON 必须返回 false");
    QVERIFY2(!err.isEmpty(), "非法 JSON 必须给出可读原因");
}

void TestSummaryParser::missingMetricsReportsReason()
{
    TestRunResult r;
    QString err;
    QVERIFY2(!SummaryParser::parseJson(QByteArrayLiteral("{\"root_group\":{}}"), &r, &err),
             "缺 metrics 对象必须返回 false");
    QVERIFY2(!err.isEmpty(), "缺 metrics 必须给出可读原因");
}

void TestSummaryParser::missingFileReportsReason()
{
    TestRunResult r;
    QString err;
    const QString ghost = TestPaths::corpusDir() + QStringLiteral("/__does_not_exist__.json");
    QVERIFY2(!SummaryParser::parseFile(ghost, &r, &err), "文件不存在必须返回 false");
    QVERIFY2(!err.isEmpty(), "文件不存在必须给出可读原因");
}

void TestSummaryParser::nullResultIsRejected()
{
    QString err;
    QVERIFY(!SummaryParser::parseJson(QByteArrayLiteral("{}"), nullptr, &err));
    QVERIFY(!SummaryParser::parseFile(Corpus::summaryClean(), nullptr, &err));
}

QTEST_GUILESS_MAIN(TestSummaryParser)
#include "tst_summaryparser.moc"
