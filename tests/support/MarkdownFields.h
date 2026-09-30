#pragma once

// ============================================================================
// 从 Markdown 报告里取字段值 —— 【唯一实现】
//
// 为什么抽出来：unit/tst_reportwriter.cpp 与 ui/tst_orchestrator.cpp 都要读
// 报告正文。各写一份的代价立刻就出现了 —— 其中一份写成【前缀匹配】，于是
// 查 "错误" 命中的是 "- 错误率: 0.01"，断言报出 "期望 5、实际 0.01"
// 这种看着像产品坏了、其实是判据坏了的失败。
//
// 两条规则：
//   ① 必须先剥掉 Markdown 的列表/标题标记。报告里每行都是 "- 脚本路径: xxx"，
//      直接 startsWith("脚本路径") 永远为 false，所有取值断言会恒等于 <缺失>、
//      恒 FAIL —— 而且方向碰巧是对的，看起来特别像被测代码的 bug。
//   ② 【整字段名精确匹配】，不是前缀匹配。"错误" 不等于 "错误率"。
// ============================================================================

#include <QString>

namespace MarkdownFields {

// 剥掉行首的 - / * / # 与其后的空白
inline QString stripMarks(const QString &line)
{
    QString t = line.trimmed();
    while (!t.isEmpty() && (t.startsWith(QLatin1Char('-'))
                            || t.startsWith(QLatin1Char('*'))
                            || t.startsWith(QLatin1Char('#'))))
        t = t.mid(1).trimmed();
    return t;
}

// 在报告里找 "字段名: 值" / "字段名 = 值" 这一类行。
// 找到返回 true 并写入 valueOut；找不到返回 false（valueOut 保持不动）。
inline bool lookup(const QString &markdown, const QString &field, QString *valueOut)
{
    const QStringList lines = markdown.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString t = stripMarks(line);

        const int colon = t.indexOf(QLatin1Char(':'));
        const int equal = t.indexOf(QLatin1Char('='));

        int cut = -1;
        if (colon >= 0 && equal >= 0) cut = qMin(colon, equal);
        else                          cut = qMax(colon, equal);

        // ★ 分隔符【之前】的那一段才是字段名，必须整体相等
        const QString name = (cut < 0) ? t : t.left(cut).trimmed();
        if (name != field) continue;

        if (valueOut) *valueOut = (cut < 0) ? t : t.mid(cut + 1).trimmed();
        return true;
    }
    return false;
}

// 取不到时返回 "<缺失>" —— 让断言里直接看得见"这一行根本没出现"
inline QString value(const QString &markdown, const QString &field)
{
    QString out;
    return lookup(markdown, field, &out) ? out : QStringLiteral("<缺失>");
}

// 区分"这一行存在但值为空"和"这一行根本没出现"
inline bool has(const QString &markdown, const QString &field)
{
    return lookup(markdown, field, nullptr);
}

}  // namespace MarkdownFields
