// ============================================================================
// 冒烟测试 —— 只验"测试基础设施本身"，不验产品逻辑。
//
// 存在的理由：接下来要往里灌一百多条真实断言。在那之前先证明
//   · Qt6::Test 链得上、CTest 认这个目标
//   · benchboard_core 链得上（真的能调到产品代码）
//   · TestPaths 指到了真实存在的文件
// 这三件事任一不成立，后面所有失败都无法区分"代码坏了"和"路没接上"。
// ============================================================================

#include <QtTest>

#include "TestPaths.h"
#include "ReportWriter.h"   // 拿一个真实符号，证明 benchboard_core 真的链上了

#include <QDateTime>
#include <QFileInfo>

class TestSmoke : public QObject
{
    Q_OBJECT

private slots:
    void infrastructureIsWired()
    {
        QVERIFY2(QFileInfo::exists(TestPaths::sourceDir() + "/CMakeLists.txt"),
                 qPrintable(QStringLiteral("源码根目录不对：%1").arg(TestPaths::sourceDir())));
        QVERIFY2(QFileInfo::exists(TestPaths::corpus("k6_stream_sample_2k.jsonl")),
                 qPrintable(QStringLiteral("语料路径不对：%1").arg(TestPaths::corpusDir())));
        QVERIFY2(QFileInfo::exists(TestPaths::src("MainWindow.cpp")),
                 "分层测试要读 src/ 下的源码，这个路径必须指对");
    }

    void coreLibraryLinks()
    {
        // 不验业务语义（那是 unit 层的事），只验这个符号真的能调到
        const QDateTime t = QDateTime::fromString(QStringLiteral("2026-09-21 15:24:00"),
                                                  QStringLiteral("yyyy-MM-dd hh:mm:ss"));
        QCOMPARE(ReportWriter::safeFileName(t), QStringLiteral("benchboard-20260921-152400.md"));
    }
};

QTEST_GUILESS_MAIN(TestSmoke)
#include "tst_smoke.moc"
