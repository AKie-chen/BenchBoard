// ============================================================================
// K6Engine —— 命令行参数拼装 + 引擎可判定状态
//
// 为什么能不起进程：buildArguments() 是【纯函数】—— 参数拼装的正确性可以在
// 不拉起 k6 的前提下逐条断言（这正是把它设计成 public static 的理由）。
//
// 判据挑【有默认值】的字段，用"等于本次输入"而不是"非空"：
//   TestConfig 的 vus 默认 10、duration 默认 "30s"，而本次输入是 7 / "60s"，
//   所以"报告里是 7"这件事不可伪造 —— "非空"则会被默认值骗过。
//
// 需要本机真的装了 k6 的两条（detectK6Path / isAvailable）在没装时 QSKIP，
// 不算失败 —— 它们验的是"环境里有没有 k6"，不是产品逻辑。
//
// 来源：tools/probes/m7_engine_probe.cpp 的 Part A + Part B。
// ============================================================================

#include <QtTest>

#include "K6Engine.h"
#include "LoadEngine.h"
#include "types.h"

#include <QStringList>

namespace {

TestConfig makeConfig()
{
    TestConfig cfg;
    cfg.targetUrl  = QStringLiteral("http://127.0.0.1:8899/");
    cfg.vus        = 7;                                    // 默认值是 10
    cfg.duration   = QStringLiteral("60s");                // 默认值是 30s
    cfg.outputDir  = QStringLiteral("C:/bench/out");
    cfg.k6Path     = QStringLiteral("C:/Program Files/k6/k6.exe");
    cfg.scriptPath = QStringLiteral("C:/bench/examples/script_demo.js");
    return cfg;
}

// 取 key 后面紧跟的那个元素（k6 的参数都是 `--key value` 成对出现）
QString after(const QStringList &l, const QString &key)
{
    const int i = l.indexOf(key);
    return (i >= 0 && i + 1 < l.size()) ? l.at(i + 1) : QStringLiteral("<缺失>");
}

}  // namespace

class TestEngine : public QObject
{
    Q_OBJECT

private slots:
    // --- Part A：buildArguments（纯函数，不起进程）---
    void invocationStartsWithRun();
    void jsonOutputGoesToStdoutNotAFile();
    void vusComesFromThisRun();
    void durationComesFromThisRun();
    void summaryExportReceivesAFilePath();
    void trendStatsIncludeP99();
    void baseUrlIsPassedAsEnvPair();
    void scriptIsLastArgument();
    void argumentCountIsExact();
    void noEmptyArguments();

    // --- Part B：引擎可判定状态 ---
    void notRunningBeforeStart();
    void engineName();
    void summaryPathIsEmptyBeforeStart();
    void detectedEngineIsConsistent();
};

// ===========================================================================
// Part A
// ===========================================================================

void TestEngine::invocationStartsWithRun()
{
    QCOMPARE(K6Engine::buildArguments(makeConfig()).value(0), QStringLiteral("run"));
}

// -o json=-  的等号不能丢：写成 json=stdout 会被 k6 当成文件名，
// 然后生成一个叫 stdout 的 58 MB 文件。
void TestEngine::jsonOutputGoesToStdoutNotAFile()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    QCOMPARE(after(args, QStringLiteral("-o")), QStringLiteral("json=-"));
}

// ★ --vus 必须等于【本次输入】7（TestConfig 默认值是 10 → 不可伪造）
//   不传这个参数，界面上的并发输入框就是摆设。
void TestEngine::vusComesFromThisRun()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    QCOMPARE(after(args, QStringLiteral("--vus")), QStringLiteral("7"));
}

// ★ --duration 必须等于【本次输入】"60s"（默认值是 "30s" → 不可伪造）
void TestEngine::durationComesFromThisRun()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    QCOMPARE(after(args, QStringLiteral("--duration")), QStringLiteral("60s"));
}

// ★ --summary-export 收的是【文件路径】而不是目录
void TestEngine::summaryExportReceivesAFilePath()
{
    const TestConfig cfg = makeConfig();
    const QStringList args = K6Engine::buildArguments(cfg);

    QCOMPARE(after(args, QStringLiteral("--summary-export")),
             cfg.outputDir + QStringLiteral("/summary.json"));
}

// 不加 --summary-trend-stats 就没有 p99（k6 默认汇总不含它）
void TestEngine::trendStatsIncludeP99()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    const QString stats = after(args, QStringLiteral("--summary-trend-stats"));

    QVERIFY2(stats.contains(QStringLiteral("p(99)")), qPrintable(stats));
}

// ★ -e KEY=VALUE 一次占【两个】元素：写成 "-e BASE_URL=..." 一整个字符串
//   会被 k6 当成一个未知参数。
void TestEngine::baseUrlIsPassedAsEnvPair()
{
    const TestConfig cfg = makeConfig();
    const QStringList args = K6Engine::buildArguments(cfg);

    QCOMPARE(after(args, QStringLiteral("-e")),
             QStringLiteral("BASE_URL=") + cfg.targetUrl);
}

void TestEngine::scriptIsLastArgument()
{
    const TestConfig cfg = makeConfig();
    QCOMPARE(K6Engine::buildArguments(cfg).last(), cfg.scriptPath);
}

// 逐个消费参数、不靠猜 —— 数量对不上就说明有参数被吃掉或重复了
void TestEngine::argumentCountIsExact()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    QCOMPARE(args.size(), 15);
}

// 空元素会被 k6 当成一个未知参数
void TestEngine::noEmptyArguments()
{
    const QStringList args = K6Engine::buildArguments(makeConfig());
    QVERIFY2(!args.contains(QString()), qPrintable(args.join(QLatin1Char('|'))));
}

// ===========================================================================
// Part B
// ===========================================================================

// ★ 不能写成 `return !m_stopping;` —— 进程压根没起来时 m_stopping 也是 false，
//   会误报"在跑"。
void TestEngine::notRunningBeforeStart()
{
    K6Engine eng;
    QVERIFY(!eng.isRunning());
}

void TestEngine::engineName()
{
    K6Engine eng;
    QCOMPARE(eng.name(), QStringLiteral("k6"));
}

void TestEngine::summaryPathIsEmptyBeforeStart()
{
    K6Engine eng;
    QVERIFY(eng.lastSummaryPath().isEmpty());
}

// 两条判据必须同源：isAvailable() 说"可用" ⇔ detectK6Path() 找到了路径。
// 本机没装 k6 时跳过 —— 这条验的是环境，不是产品逻辑。
void TestEngine::detectedEngineIsConsistent()
{
    if (K6Engine::detectK6Path().isEmpty())
        QSKIP("本机未安装 k6：跳过引擎探测一致性检查（不是失败）");

    QVERIFY2(!K6Engine::detectK6Path().isEmpty(), "detectK6Path 应当找到 k6");

    K6Engine eng;
    QString err;
    QVERIFY2(eng.isAvailable(&err),
             qPrintable(QStringLiteral("isAvailable 与 detectK6Path 结论不一致：%1").arg(err)));
    QVERIFY(err.isEmpty());
}

QTEST_GUILESS_MAIN(TestEngine)
#include "tst_engine.moc"
