// SummaryParser 单测 —— 直接喂 JSON，不依赖界面、不依赖 QProcess
#include "SummaryParser.h"

#include <QCoreApplication>
#include <cstdio>

static int failures = 0;

static void expectLL(const char *what, long long got, long long want)
{
    const bool ok = (got == want);
    if (!ok) ++failures;
    std::printf("  [%s] %-16s got=%lld want=%lld\n", ok ? "PASS" : "FAIL", what, got, want);
}

static void expectD(const char *what, double got, double want)
{
    const bool ok = (qAbs(got - want) < 1e-3);
    if (!ok) ++failures;
    std::printf("  [%s] %-16s got=%.4f want=%.4f\n", ok ? "PASS" : "FAIL", what, got, want);
}

static void runCase(const QString &path, bool wantOk)
{
    std::printf("\n--- %s ---\n", qPrintable(path));
    TestRunResult r;
    QString err;
    const bool ok = SummaryParser::parseFile(path, &r, &err);
    std::printf("  [%s] parseFile 返回 %s%s%s\n", (ok == wantOk) ? "PASS" : "FAIL",
                ok ? "true" : "false",
                err.isEmpty() ? "" : "  err=", qPrintable(err));
    if (ok != wantOk) ++failures;
    if (!ok) return;

    std::printf("  totalRequests=%lld  rps=%.2f  errorRate=%.4f  checks=%lld\n",
                (long long)r.totalRequests, r.rps, r.errorRate, (long long)r.checks.size());
    std::printf("  min/avg/med/max = %.4f / %.4f / %.4f / %.4f\n",
                r.minDurationMs, r.avgDurationMs, r.medDurationMs, r.maxDurationMs);
    std::printf("  p90/p95/p99     = %.4f / %.4f / %.4f\n",
                r.p90DurationMs, r.p95DurationMs, r.p99DurationMs);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // ---------------------------------------------------------------
    // 用例 1：语料样本（干净结构，没有 thresholds 子对象）
    // ---------------------------------------------------------------
    std::printf("\n========== 用例 1：语料样本 ==========\n");
    {
        TestRunResult r;
        QString err;
        const QString p = QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_summary_sample.json");
        const bool ok = SummaryParser::parseFile(p, &r, &err);
        std::printf("  [%s] parseFile 返回 %s  err=%s\n", ok ? "PASS" : "FAIL",
                    ok ? "true" : "false", qPrintable(err));
        if (!ok) ++failures;
        else {
            expectLL("totalRequests", r.totalRequests, 4375);
            expectD ("avgDurationMs", r.avgDurationMs, 3.0926);
            expectD ("p95DurationMs", r.p95DurationMs, 4.0206);
            expectD ("p99DurationMs", r.p99DurationMs, 4.8286);
            expectD ("errorRate",     r.errorRate,     0.0);
            expectLL("checks 条数",   r.checks.size(), 1);
            if (!r.checks.isEmpty())
                expectLL("checks[0].passes", r.checks[0].passes, 4375);
        }
    }

    // ---------------------------------------------------------------
    // 用例 2：真实产出形态 —— http_req_duration 里【混着 thresholds 子对象】
    //         语料样本里没有这个键；不判类型就会把 thresholds 当成数值读。
    // ---------------------------------------------------------------
    runCase(QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_summary_with_thresholds.json"), true);
    {
        TestRunResult r;
        QString err;
        SummaryParser::parseFile(QStringLiteral(
            BENCHBOARD_ROOT "/tests/corpus/k6_summary_with_thresholds.json"), &r, &err);
        expectLL("totalRequests", r.totalRequests, 1112);
        expectD ("avgDurationMs", r.avgDurationMs, 8.2235);
        expectD ("p95DurationMs", r.p95DurationMs, 10.4512);
        expectD ("p99DurationMs", r.p99DurationMs, 12.1281);
        expectLL("checks 条数",   r.checks.size(), 1);
    }

    // ---------------------------------------------------------------
    // 用例 3：缺 p(99) —— 必须是「解析成功但该字段留 0」，不能整体失败。
    //         （对应"缺字段显示 — 而不是 0"这条要求：失败与缺字段是两件事）
    // ---------------------------------------------------------------
    runCase(QStringLiteral(BENCHBOARD_ROOT "/tests/corpus/k6_summary_missing_p99.json"), true);
    {
        TestRunResult r;
        QString err;
        SummaryParser::parseFile(QStringLiteral(
            BENCHBOARD_ROOT "/tests/corpus/k6_summary_missing_p99.json"), &r, &err);
        expectLL("totalRequests", r.totalRequests, 100);
        expectD ("avgDurationMs", r.avgDurationMs, 1.5);
        expectD ("p99DurationMs", r.p99DurationMs, 0.0);   // 缺 → 保持默认，显示层画 "—"
    }

    // ---------------------------------------------------------------
    // 用例 4：坏输入必须【失败】并且给出可读原因
    // ---------------------------------------------------------------
    std::printf("\n--- 用例 4：坏输入 ---\n");
    {
        TestRunResult r;
        QString err;
        const bool ok = SummaryParser::parseJson(QByteArrayLiteral("{ not json"), &r, &err);
        std::printf("  [%s] 非法 JSON → %s，err=\"%s\"\n", !ok && !err.isEmpty() ? "PASS" : "FAIL",
                    ok ? "true" : "false", qPrintable(err));
        if (ok || err.isEmpty()) ++failures;
    }
    {
        TestRunResult r;
        QString err;
        const bool ok = SummaryParser::parseJson(QByteArrayLiteral("{\"root_group\":{}}"), &r, &err);
        std::printf("  [%s] 缺 metrics → %s，err=\"%s\"\n", !ok && !err.isEmpty() ? "PASS" : "FAIL",
                    ok ? "true" : "false", qPrintable(err));
        if (ok || err.isEmpty()) ++failures;
    }

    std::printf("\n================ 结果 ================\n");
    std::printf("FAIL 数 = %d\n", failures);
    std::printf(failures == 0 ? "RESULT=ALL_PASS\n" : "RESULT=HAS_FAILURE\n");
    return failures == 0 ? 0 : 1;
}
