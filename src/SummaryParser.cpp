#include "SummaryParser.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

// ============================================================================
// parseFile / parseJson 实现
//
// ★ 最容易错的地方是【层级】：k6 的 summary.json 是两层结构
//     { "root_group": {...}, "metrics": { "<指标名>": { <字段...> } } }
//   avg / p(95) / max 这些**不在 metrics 顶层**，而在
//   metrics["http_req_duration"] 这个子对象里。取错层级不会报错，
//   只会让 takeDouble 恒返回 false，整个解析静默失败。
//
// ★ 第二个坑：metrics["http_req_duration"] 里可能混进非数值键。
//   实测 app 真实产出（reports/summary.json:56-58）该对象里有
//     "thresholds": { "p(95)<500": false }
//   而 tests/corpus/k6_summary_sample.json 里【没有】。
//   → 取值一律走 takeDouble()，它用 isDouble() 过滤；
//     语料单测全绿 ≠ 真实数据正确。
//
// ★ 第三个坑：root_group.checks 是【对象】不是数组：
//     "checks": { "status is 200": { "passes": 4375, "fails": 0, ... } }
//   调 toArray() 得到空数组 → 循环 0 次 → checks 永远为空，零报错。
// ============================================================================

bool SummaryParser::parseFile(const QString& path, TestRunResult* result, QString* errorOut)
{
    if (!result) return false;

    QFile file(path);
    // ★ QFile::open() 带 [[nodiscard]] —— 不判返回值就是 C4834，破坏"0 警告"验收
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorOut) *errorOut = QStringLiteral("无法打开文件：%1").arg(path);
        return false;
    }
    return parseJson(file.readAll(), result, errorOut);
}

bool SummaryParser::parseJson(const QByteArray& json, TestRunResult* result, QString* errorOut)
{
    if (!result) return false;

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // ★ 可读原因必须在这里写出去：调用方靠它区分"文件坏了"和"字段缺了"。
        //   之前写成了 else 分支，结果是"解析成功反而报错、解析失败反而没原因"。
        if (errorOut) *errorOut = err.errorString();
        return false;
    }

    const QJsonObject obj     = doc.object();
    const QJsonObject metrics = obj.value(QStringLiteral("metrics")).toObject();
    if (metrics.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("JSON 里没有 metrics 对象");
        return false;
    }

    // --- 耗时类：全部在 metrics["http_req_duration"] 里（★ 不是 metrics 顶层）---
    // ★ 逐个取，【不要】用 || 串联：串联等于"任意一个字段缺失就整体失败"，
    //   而本项目的要求恰恰相反 —— 缺字段显示 "—"，不能让整次解析作废。
    //   takeDouble 取不到时不写 out，值保持默认 0.0，由显示层决定画成 "—"。
    const QJsonObject dur = metrics.value(QStringLiteral("http_req_duration")).toObject();
    takeDouble(dur, "avg",   &result->avgDurationMs);
    takeDouble(dur, "min",   &result->minDurationMs);
    takeDouble(dur, "med",   &result->medDurationMs);
    takeDouble(dur, "max",   &result->maxDurationMs);
    takeDouble(dur, "p(90)", &result->p90DurationMs);
    takeDouble(dur, "p(95)", &result->p95DurationMs);
    takeP99(dur, &result->p99DurationMs);      // ★ 走 takeP99，别绕过这个契约函数

    // --- counter：summary 里是【累计值】（与原始流里的 delta 语义相反）---
    const QJsonObject reqs = metrics.value(QStringLiteral("http_reqs")).toObject();
    result->totalRequests = qint64(reqs.value(QStringLiteral("count")).toDouble());
    result->rps           = reqs.value(QStringLiteral("rate")).toDouble();

    // --- rate：http_req_failed.value 就是失败占比（0.0 ~ 1.0）---
    // ★ 别用它的 passes / fails —— 实测两者语义是反的
    //   （passes = 失败数 0，fails = 成功数 = http_reqs.count），用 value 最稳。
    result->errorRate = metrics.value(QStringLiteral("http_req_failed"))
                            .toObject()
                            .value(QStringLiteral("value"))
                            .toDouble();

    // --- checks：root_group.checks 是【对象】，遍历它的 key/value ---
    const QJsonObject checksObj = obj.value(QStringLiteral("root_group"))
                                     .toObject()
                                     .value(QStringLiteral("checks"))
                                     .toObject();
    if (!checksObj.isEmpty()) {
        result->checks.clear();
        for (auto it = checksObj.constBegin(); it != checksObj.constEnd(); ++it) {
            const QJsonObject c = it.value().toObject();
            CheckResult cr;
            // name 通常与 key 相同；缺了就退回用 key，别留空
            cr.name   = c.value(QStringLiteral("name")).toString(it.key());
            cr.passes = qint64(c.value(QStringLiteral("passes")).toDouble());
            cr.fails  = qint64(c.value(QStringLiteral("fails")).toDouble());
            result->checks.append(cr);
        }
    }

    // 注意：summary.json 顶层【没有】 state / engineVersion，
    // 所以"跑完没跑完"不能靠这个文件判断 —— 只能靠 QProcess::exitStatus。
    return true;
}

bool SummaryParser::takeDouble(const QJsonObject& metric, const char* key, double* out)
{
    if (!out || !key || metric.isEmpty()) return false;

    const QJsonValue v = metric.value(QLatin1StringView(key));
    // ★ isDouble() 一次挡掉两种"取不到"：键不存在、键存在但值是子对象（thresholds）
    if (!v.isDouble()) return false;

    *out = v.toDouble();
    return true;
}

bool SummaryParser::takeP99(const QJsonObject& metric, double* out)
{
    // 单独留一个函数，是为了让"p(99) 可能不存在"这件事在调用处看得见：
    // 它只在传了 --summary-trend-stats 时才产出（本 app 一直传着）。
    return takeDouble(metric, "p(99)", out);
}
