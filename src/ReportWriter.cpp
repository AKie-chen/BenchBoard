#include "ReportWriter.h"
#include <QDateTime>
#include <QDir>

// 生成 Markdown 报告，返回实际写入的绝对路径；失败时通过 errorOut 返回原因
QString ReportWriter::writeMarkdown(const TestRunResult& result, const QString& outputDir, QString* errorOut)
{
    QString fileName = safeFileName(result.startedAt);
    QString filePath = QDir(outputDir).filePath(fileName);
    // 修正 mkpath 调用，确保 outputDir 存在
    if (!QDir().mkpath(outputDir)) {
        if (errorOut) errorOut->append("无法创建目录: " + outputDir);
        return QString();
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (errorOut) errorOut->append("无法创建文件: " + file.errorString());
        return QString();
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(false);

    out << buildMarkdown(result);
    file.close();

	return filePath;
}

// 生成文件名（不含目录），形如 benchboard-20260921-152400.md
// 只使用数字、连字符与固定前缀，天然免疫路径穿越
QString ReportWriter::safeFileName(const QDateTime& timestamp)
{
    return timestamp.isValid() ? QString("benchboard-%1.md").arg(timestamp.toString("yyyyMMdd-hhmmss")) : QStringLiteral("benchboard-unknow.md");
}

// 组装 Markdown 正文（拆出来是为了能单测，不落盘也能断言内容）
QString ReportWriter::buildMarkdown(const TestRunResult& result)
{
    // 显示用的小工具：耗时 0 一律显示 "—"，不显示 "0.00 ms"。
    const auto fmtMs = [](double v) -> QString {
        return v > 0.0 ? QStringLiteral("%1 ms").arg(v, 0, 'f', 2)
            : QStringLiteral("—");
    };

    QString text;
    text += QStringLiteral("# BenchBoard 压测报告\n\n");

    text += QStringLiteral("## 压测配置\n\n");
    text += QStringLiteral("- 目标 URL: %1\n").arg(result.config.targetUrl);
    text += QStringLiteral("- 脚本路径: %1\n").arg(result.config.scriptPath);
    text += QStringLiteral("- vus: %1\n").arg(result.config.vus);
    text += QStringLiteral("- duration: %1\n").arg(result.config.duration);
    text += QStringLiteral("- k6 路径: %1\n").arg(result.config.k6Path);
    text += QStringLiteral("- k6 版本: %1\n").arg(result.engineVersion);
    text += QStringLiteral("- 输出目录: %1\n\n").arg(result.config.outputDir);

    text += QStringLiteral("## 压测结果\n\n");
    text += QStringLiteral("- 开始时间: %1\n").arg(result.startedAt.toString("yyyy-MM-dd hh:mm:ss"));
    text += QStringLiteral("- 总请求数: %1\n").arg(result.totalRequests);
    text += QStringLiteral("- 平均 RPS: %1\n").arg(result.rps, 0, 'f', 2);
    // 使用 fmtMs 输出耗时字段，fmtMs 已包含单位或占位符
    text += QStringLiteral("- 最小耗时: %1\n").arg(fmtMs(result.minDurationMs));
    text += QStringLiteral("- 平均耗时: %1\n").arg(fmtMs(result.avgDurationMs));
    text += QStringLiteral("- 中位数耗时: %1\n").arg(fmtMs(result.medDurationMs));
    text += QStringLiteral("- 最大耗时: %1\n").arg(fmtMs(result.maxDurationMs));
    text += QStringLiteral("- 90% 耗时: %1\n").arg(fmtMs(result.p90DurationMs));
    text += QStringLiteral("- 95% 耗时: %1\n").arg(fmtMs(result.p95DurationMs));
    text += QStringLiteral("- 99% 耗时: %1\n").arg(fmtMs(result.p99DurationMs));
    text += QStringLiteral("- 错误率: %1\n\n").arg(result.errorRate, 0, 'f', 2);

    for (const auto& check : result.checks) {
        text += QStringLiteral("### 检查项: %1\n\n").arg(check.name);
        text += QStringLiteral("- 通过: %1\n").arg(check.passes);
        text += QStringLiteral("- 错误: %1\n\n").arg(check.fails);
    }
    return text;
}
