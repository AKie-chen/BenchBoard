// ============================================================================
// ReportWriter —— Markdown 报告的契约
//
// 三组：
//   A  文件名：格式 + 路径穿越免疫 + 无效时间戳的兜底
//   B  正文  ：内容契约（独立成行 / 2 位小数 / checks / 缺失值写 "—" 不写 0）
//   C  落盘  ：编码无 BOM / 换行是 \n / 大内容不截断 / 【失败必须让调用方察觉】
//
// 判据的写法原则：凡是"不报错也能写错"的地方，都要有一条能【翻红】的断言。
// 重点是 C4 与 C5：
//   C4  内容超过 QTextStream 内部缓冲（16KB）时，file.close() 早于 out 析构会丢尾巴
//   C5  目录建不出来时，如果照样返回一个路径，调用方无法察觉
//
// 来源：tools/probes/m6_report_probe.cpp 的 Part A/B/C。
// 与探针的差别：全部落在 QTemporaryDir 里，不再借用仓库的 reports/ 目录，
// 也就不需要"跑完自己删产物"那套记账。
// ============================================================================

#include <QtTest>

#include "Corpus.h"
#include "MarkdownFields.h"
#include "ReportWriter.h"
#include "TestPaths.h"
#include "types.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace {

// 造一份"填满"的 TestRunResult —— 所有字段都有值，用来验内容契约
TestRunResult makeFull()
{
    TestRunResult r;
    r.config.targetUrl  = QStringLiteral("http://127.0.0.1:8899/");
    r.config.scriptPath = TestPaths::sourceDir() + QStringLiteral("/examples/script_demo.js");
    r.config.vus        = 5;
    r.config.duration   = QStringLiteral("30s");
    r.config.k6Path     = QStringLiteral("C:/k6/k6.exe");
    r.config.outputDir  = TestPaths::sourceDir();
    r.startedAt         = QDateTime::fromString(QStringLiteral("2026-09-21 15:24:00"),
                                                QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    r.totalRequests = 12345;
    r.rps           = 197.649123;
    r.minDurationMs = 18.7905;
    r.avgDurationMs = 39.8702;
    r.medDurationMs = 39.8204;
    r.maxDurationMs = 56.0349;
    r.p90DurationMs = 44.3863;
    r.p95DurationMs = 45.8747;
    r.p99DurationMs = 48.9699;
    r.errorRate     = 0.0123;
    r.engineVersion = QStringLiteral("k6 v2.2.0");

    CheckResult c1;
    c1.name   = QStringLiteral("status is 200");
    c1.passes = 12340;
    c1.fails  = 5;
    r.checks.append(c1);
    return r;
}

}  // namespace

class TestReportWriter : public QObject
{
    Q_OBJECT

private slots:
    // --- A 文件名 ---
    void safeFileNameFormat();
    void safeFileNameHasNoPathSeparators();
    void safeFileNameFallsBackWhenTimestampInvalid();

    // --- B 正文 ---
    void fieldsAreOnTheirOwnLines();
    void numbersUseTwoDecimals();
    void checksAppearInReport();
    void missingDurationsRenderAsDashNotZero();
    void configSectionCarriesInputValues();

    // --- C 落盘 ---
    void writesFileAndLeavesErrorEmpty();
    void writesUtf8WithoutBom();
    void writesLfNotCrLf();
    void largeContentIsNotTruncated();
    void uncreatableDirectoryIsReported();
};

// ===========================================================================
// A 文件名
// ===========================================================================

void TestReportWriter::safeFileNameFormat()
{
    const QDateTime valid = QDateTime::fromString(QStringLiteral("2026-09-21 15:24:00"),
                                                  QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    QCOMPARE(ReportWriter::safeFileName(valid), QStringLiteral("benchboard-20260921-152400.md"));
}

void TestReportWriter::safeFileNameHasNoPathSeparators()
{
    const QDateTime valid = QDateTime::fromString(QStringLiteral("2026-09-21 15:24:00"),
                                                  QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    const QString name = ReportWriter::safeFileName(valid);

    QVERIFY2(!name.contains(QLatin1Char('/')),  qPrintable(name));
    QVERIFY2(!name.contains(QLatin1Char('\\')), qPrintable(name));
    QVERIFY2(!name.contains(QStringLiteral("..")), qPrintable(name));
}

void TestReportWriter::safeFileNameFallsBackWhenTimestampInvalid()
{
    const QDateTime invalid;   // 默认构造 = 无效时间戳
    const QString name = ReportWriter::safeFileName(invalid);

    QVERIFY2(!name.isEmpty(), "无效时间戳必须仍有文件名");
    QVERIFY2(name != QStringLiteral("benchboard-.md"), qPrintable(name));
}

// ===========================================================================
// B 正文
// ===========================================================================

// "vu = 5duration = 30s" 这种粘连是少写一个 \n 的典型症状
void TestReportWriter::fieldsAreOnTheirOwnLines()
{
    const QString md = ReportWriter::buildMarkdown(makeFull());

    static const QRegularExpression kSticky(QStringLiteral("vu\\s*=\\s*\\d+\\s*duration"));
    QVERIFY2(!kSticky.match(md).hasMatch(),
             qPrintable(QStringLiteral("vus 行 = \"%1\"")
                            .arg(MarkdownFields::value(md,QStringLiteral("vus")))));
}

// 头文件契约写着「数值统一保留 2 位小数」
void TestReportWriter::numbersUseTwoDecimals()
{
    const QString md = ReportWriter::buildMarkdown(makeFull());

    static const QRegularExpression kLongDec(QStringLiteral("\\d+\\.\\d{3,}"));
    const QRegularExpressionMatch m = kLongDec.match(md);
    QVERIFY2(!m.hasMatch(),
             qPrintable(QStringLiteral("首个越界值 = %1").arg(m.captured(0))));
}

void TestReportWriter::checksAppearInReport()
{
    const TestRunResult full = makeFull();
    const QString md = ReportWriter::buildMarkdown(full);

    QVERIFY2(md.contains(QStringLiteral("status is 200")),
             qPrintable(QStringLiteral("checks.size()=%1，报告里找不到该 check 名")
                            .arg(full.checks.size())));
    QCOMPARE(MarkdownFields::value(md,QStringLiteral("通过")), QStringLiteral("12340"));
    QCOMPARE(MarkdownFields::value(md,QStringLiteral("错误")), QStringLiteral("5"));
}

// ★ 契约：缺失值写 "—"，不能写 0（同族于汇总表格里的 fmtMs）
void TestReportWriter::missingDurationsRenderAsDashNotZero()
{
    TestRunResult r = makeFull();
    r.minDurationMs = 0.0;   // 强杀兜底时这些字段就是 0
    r.p95DurationMs = 0.0;

    const QString md = ReportWriter::buildMarkdown(r);

    const QString minVal = MarkdownFields::value(md,QStringLiteral("最小耗时"));
    const QString p95Val = MarkdownFields::value(md,QStringLiteral("95% 耗时"));

    QVERIFY2(minVal.startsWith(QStringLiteral("—")),
             qPrintable(QStringLiteral("最小耗时 = \"%1\"").arg(minVal)));
    QVERIFY2(p95Val.startsWith(QStringLiteral("—")),
             qPrintable(QStringLiteral("95%% 耗时 = \"%1\"").arg(p95Val)));
}

void TestReportWriter::configSectionCarriesInputValues()
{
    const TestRunResult full = makeFull();
    const QString md = ReportWriter::buildMarkdown(full);

    QCOMPARE(MarkdownFields::value(md,QStringLiteral("目标 URL")), full.config.targetUrl);
    QCOMPARE(MarkdownFields::value(md,QStringLiteral("vus")),      QStringLiteral("5"));
    QCOMPARE(MarkdownFields::value(md,QStringLiteral("duration")), QStringLiteral("30s"));
    QVERIFY(md.contains(QStringLiteral("k6 v2.2.0")));
}

// ===========================================================================
// C 落盘
// ===========================================================================

void TestReportWriter::writesFileAndLeavesErrorEmpty()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    QString err;
    const QString p = ReportWriter::writeMarkdown(makeFull(), tmp.path(), &err);

    QVERIFY2(!p.isEmpty() && QFile::exists(p),
             qPrintable(QStringLiteral("返回 = \"%1\"  err = \"%2\"").arg(p, err)));
    QVERIFY2(err.isEmpty(), qPrintable(QStringLiteral("成功时 errorOut 应为空，实际 = \"%1\"").arg(err)));

    // 返回路径风格统一（QDir::filePath 会归一化分隔符）
    QVERIFY2(!p.contains(QLatin1Char('\\')), qPrintable(p));
}

// 契约：UTF-8 【无 BOM】
void TestReportWriter::writesUtf8WithoutBom()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString p = ReportWriter::writeMarkdown(makeFull(), tmp.path());
    QVERIFY(!p.isEmpty());

    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray head = f.read(3);

    QVERIFY2(head != QByteArray("\xEF\xBB\xBF", 3),
             qPrintable(QStringLiteral("前三字节 = %1").arg(QString::fromLatin1(head.toHex(' ')))));
}

// 契约：换行是 \n —— 开着 QIODevice::Text 会被 Qt 转成 CRLF
void TestReportWriter::writesLfNotCrLf()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString p = ReportWriter::writeMarkdown(makeFull(), tmp.path());
    QVERIFY(!p.isEmpty());

    QFile f(p);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray body = f.readAll();

    QCOMPARE(body.count('\r'), 0);
}

// ★ 内容超过 QTextStream 内部 16KB 缓冲时，file.close() 早于 out 析构会丢尾巴。
//   判据不猜内容，直接对账字节数。
void TestReportWriter::largeContentIsNotTruncated()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    TestRunResult big = makeFull();
    big.startedAt = big.startedAt.addSecs(1);      // 换个文件名，不与上一份混淆
    big.config.scriptPath = QStringLiteral("C:/very/long/path/segment/").repeated(1200)
                            + QStringLiteral("script.js");

    const QString bigMd   = ReportWriter::buildMarkdown(big);
    const QString bigPath = QDir(tmp.path()).filePath(ReportWriter::safeFileName(big.startedAt));

    QString err;
    QVERIFY(!ReportWriter::writeMarkdown(big, tmp.path(), &err).isEmpty());

    QFile f(bigPath);
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(bigPath));
    const QByteArray body = f.readAll();

    QByteArray norm = body;
    norm.replace("\r\n", "\n");

    const qint64 expect = bigMd.toUtf8().size();
    QVERIFY2(expect > 20000,
             qPrintable(QStringLiteral("用例前提不成立：正文只有 %1 B，没跨过 16KB 缓冲")
                            .arg(expect)));
    QVERIFY2(norm.size() == expect,
             qPrintable(QStringLiteral("落盘 %1 B  预期 %2 B —— 尾巴被丢了")
                            .arg(norm.size()).arg(expect)));
}

// ★ 失败路径：outputDir 指向一个【已存在的文件】，目录永远建不出来。
//   这时如果照样返回一个路径，调用方无法察觉。
void TestReportWriter::uncreatableDirectoryIsReported()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());

    const QString blocker = tmp.path() + QStringLiteral("/blocker.txt");
    {
        QFile bf(blocker);
        QVERIFY(bf.open(QIODevice::WriteOnly));
        bf.write("x");
        bf.close();
    }

    QString err;
    const QString p = ReportWriter::writeMarkdown(makeFull(), blocker, &err);

    QVERIFY2(p.isEmpty(),
             qPrintable(QStringLiteral("目录建不出来时必须返回空串，实际 = \"%1\"").arg(p)));
    QVERIFY2(!err.isEmpty(), "失败时必须写出可读原因");
}

QTEST_GUILESS_MAIN(TestReportWriter)
#include "tst_reportwriter.moc"
