#pragma once

// ============================================================================
// 测试用的路径唯一来源
//
// 规则：测试代码里【不允许出现绝对路径】。要什么路径，从这里取。
// 两个根目录由 CMake 的 benchboard_test_support 接口目标以编译期定义传入，
// 值来自 ${CMAKE_SOURCE_DIR} —— 换机器 / 换 clone 路径都不用改代码。
// ============================================================================

#include <QString>

namespace TestPaths {

// 仓库根目录
inline QString sourceDir() { return QStringLiteral(BENCHBOARD_SOURCE_DIR); }

// 语料目录（tests/corpus）
inline QString corpusDir() { return QStringLiteral(BENCHBOARD_CORPUS_DIR); }

// 产品源码目录（分层测试要读 src/*.cpp 做静态断言）
inline QString srcDir() { return sourceDir() + QStringLiteral("/src"); }

// 语料文件完整路径：corpus("k6_stream_sample_2k.jsonl")
inline QString corpus(const QString &fileName)
{
    return corpusDir() + QLatin1Char('/') + fileName;
}

// 产品源码文件完整路径：src("MainWindow.cpp")
inline QString src(const QString &fileName)
{
    return srcDir() + QLatin1Char('/') + fileName;
}

}  // namespace TestPaths
