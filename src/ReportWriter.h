#pragma once

// ============================================================================
// Markdown 报告生成
//
// 落盘要求：
//   - 编码 UTF-8 【无 BOM】（QFile + QTextStream 默认就是这个组合）
//   - 换行用 \n（Qt 6 的 QTextStream 默认不再自动转 CRLF，但写死更保险）
//   - 数值统一保留 2 位小数；缺失值写 "—"，不能写 0
//
// 安全：文件名由 safeFileName() 白名单化，禁止把用户输入直接拼进路径，
//       否则构造 URL 里的 ..\..\ 就能越权写文件。
// ============================================================================

#include "types.h"

#include <QDateTime>
#include <QString>

class ReportWriter
{
public:
    // 生成 Markdown 报告，返回实际写入的绝对路径；失败时通过 errorOut 返回原因
    static QString writeMarkdown(const TestRunResult& result,
        const QString& outputDir,
        QString* errorOut = nullptr);

    // 生成文件名（不含目录），形如 benchboard-20260921-152400.md
    // 只使用数字、连字符与固定前缀，天然免疫路径穿越
    static QString safeFileName(const QDateTime& timestamp);

    // 组装 Markdown 正文（拆出来是为了能单测，不落盘也能断言内容）
    static QString buildMarkdown(const TestRunResult& result);
};
