#pragma once

// ============================================================================
// 语料与对账期望值的【唯一来源】
//
// 规则：测试里不写裸数字。2000 / 14 / 1986 / 142 这些值只在这里出现一次，
// 断言引用 Corpus::Expectation 的字段 —— 换语料时改一处，不用全仓 grep 数字。
//
// 期望值来源（对 tests/corpus/k6_stream_sample_2k.jsonl 的实测统计）：
//   2000 行 = 14 条 Metric 声明 + 1986 条 Point
//   含 "metric":"http_reqs" 的行有 143 条 = 1 条声明 + 142 条 Point
//     → counter 在【原始流】里发的是增量，每个请求一个 value=1 的点，累加得 142
//   语料全是合法 JSON —— 所以 parseErrors 期望是 0；
//   脏行（流尾部的人类可读摘要）另有用例单独覆盖。
// ============================================================================

#include "TestPaths.h"

#include <QByteArray>
#include <QFile>
#include <QString>

namespace Corpus {

// 2k 流语料的对账期望值
struct Expectation {
    qint64 lines       = 2000;
    qint64 metricDecls = 14;
    qint64 points      = 1986;
    qint64 requests    = 142;
    qint64 failedCount = 0;
    qint64 parseErrors = 0;
};

// --- 语料文件 ---
inline QString streamFile()       { return TestPaths::corpus("k6_stream_sample_2k.jsonl"); }
inline QString summaryClean()     { return TestPaths::corpus("k6_summary_sample.json"); }
inline QString summaryThreshold() { return TestPaths::corpus("k6_summary_with_thresholds.json"); }
inline QString summaryMissingP99(){ return TestPaths::corpus("k6_summary_missing_p99.json"); }

// 读语料原文。读不到返回空 —— 由调用方的断言负责报"语料缺失"，
// 这里不抛异常、也不用兜底数据掩盖问题。
inline QByteArray readStream()
{
    QFile f(streamFile());
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

}  // namespace Corpus
