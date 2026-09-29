#pragma once

// ============================================================================
// k6 --summary-export 产出 JSON 的解析器
//
// ★ 最容易踩的坑：k6 v2.2.0 的 metric 对象【没有 type 字段】。
//   老版本(0.x)是 {"type":"trend","contains":"time","avg":...,"p(95)":...}，
//   而 v2.2.0 实测是扁平的 {"avg":...,"min":...,"p(95)":...}。
//   所以判断指标类型只能靠【指标名 + 字段存在性】：
//     - 含 rate            → counter
//     - 含 p(95)/avg/min   → trend
//     - 含 passes/fails    → check 或 rate
//
// 实测样本（可直接用 tests/corpus/k6_summary_sample.json 做单测）：
//   {
//     "metrics": {
//       "http_reqs":         {"count": 4375, "rate": 874.77},
//       "http_req_duration": {"avg":3.09,"min":1.04,"med":3.07,"max":10.26,
//                             "p(90)":3.74,"p(95)":4.02,"p(99)":4.83,"p(99.9)":8.22},
//       "checks":            {"passes":4375,"fails":0,"value":1},
//       "http_req_failed":   {"fails":7062,"passes":0,"value":0}
//     },
//     "root_group": {"checks": {"status is 200": {"passes":4375,"fails":0}}}
//   }
//
// 注意 p(99) 只在显式传了 --summary-trend-stats 时才存在；
// 缺字段要显示 "—"，不能悄悄当成 0。
// ============================================================================

#include "types.h"

#include <QByteArray>
#include <QJsonObject>
#include <QString>

class SummaryParser
{
public:
    // 从文件读取并解析。失败时把可读原因（文件不存在 / JSON 非法 / 缺 metrics）写入 errorOut。
    static bool parseFile(const QString& path, TestRunResult* result, QString* errorOut = nullptr);

    // 便于单测：直接喂 JSON 文本
    static bool parseJson(const QByteArray& json, TestRunResult* result, QString* errorOut);

private:
    // 从 trend 型 metric 对象里取分位数值；键不存在返回 false（保持 result 原值）
    static bool takeDouble(const QJsonObject& metric, const char* key, double* out);

    // 取 p(99)；键不存在返回 false，由调用方决定怎么显示（"—" 而不是 0）
    static bool takeP99(const QJsonObject& metric, double* out);
};
