// ============================================================================
// 界面 → 编排层 → 报告 的端到端接线（离屏运行）
//
// 这是【契约测试】，不是单元测试：它靠两个技巧驱动界面，内部结构一变就要同步改。
// 换来的是一批"不可伪造"的判据 —— 报告里的 vus 必须等于本次输入 7，
// 而 TestConfig 的默认值是 10、QSpinBox 的初值是 1，所以"是 7"这件事
// 只可能来自真实的接线，不可能是默认值蒙对的。
//
// 驱动方式（两处都刻意避开真起 k6 进程）：
//   ① 把 URL 置空后点「开始压测」—— startTest() 先记 config、再 preflight，
//      而"URL 为空"在 preflight 里早退【早于】engine->start()。
//      副作用为零，且 m_config 已经填好 —— 这正是导出报告需要的前置状态。
//      不用"独特 URL"是因为那会一路走到 K6Engine::start()，真的创建 k6 子进程；
//      孤儿 k6 会在测试退出【之后】继续跑并回写 summary.json。
//   ② 用 QMetaObject::invokeMethod 直调编排层的私有槽 onEngineFinished ——
//      模拟"引擎跑完了"这一刻，不需要真的跑。
//
// 产物落在 <测试 exe 同级>/reports/（在构建目录里，不进仓库）。
// initTestCase 记账、cleanupTestCase 复原 —— 只删本次自己创建的东西。
//
// 来源：tools/probes/m6_report_probe.cpp 的 Part D/E。
// ============================================================================

#include <QtTest>

#include "Corpus.h"
#include "MainWindow.h"
#include "MarkdownFields.h"
#include "TestOrchestrator.h"
#include "types.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>

namespace {

// 本次输入的两个"不可伪造"值：
//   QSpinBox 初值 = 1（buildUi 里 setRange(1,1000)）、combo 初值 = index 0（"10s"）
//   TestConfig 的默认值 = 10 / "30s"
// 取 7 / "60s"，与三者都不同 —— 报告里出现它，只可能是接线通了。
constexpr int kUniqueVus = 7;
const QString kUniqueDur = QStringLiteral("60s");

// 界面把报告目录定在 <exe 同级>/reports（MainWindow.cpp 的 kReportsDir）
QString reportsDir()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/reports");
}

QString summaryPath()
{
    return reportsDir() + QStringLiteral("/summary.json");
}

// 目录里所有 .md 的「路径|mtime|size」快照，用于检测"这一次有没有写出东西"。
//
// ★ 三个字段缺一不可：
//   · 只比"文件名集合" → 检测不到【同名覆盖】；
//   · 只比 mtime      → 两次写入落在同一毫秒时检测不到；
//   · 加 size         → 覆盖时内容长度必然变，稳。
QStringList snapMd(const QString &dir)
{
    QStringList out;
    const QFileInfoList fs = QDir(dir).entryInfoList({QStringLiteral("*.md")},
                                                     QDir::Files, QDir::Name);
    for (const QFileInfo &fi : fs)
        out << fi.absoluteFilePath() + QLatin1Char('|')
                   + fi.lastModified().toString(Qt::ISODateWithMs)
                   + QLatin1Char('|')
                   + QString::number(fi.size());
    return out;
}

QString producedSince(const QStringList &before, const QStringList &after)
{
    for (const QString &s : after)
        if (!before.contains(s)) return s.section(QLatin1Char('|'), 0, 0);
    return QString();
}

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// 把语料装成 summary.json，模拟"k6 正常跑完，汇总文件就在那儿"。
//
// ★ 必须先 remove：QFile::copy 【不覆盖】已存在的目标。
//   QTest 的 cleanupTestCase 在所有测试函数跑完【之后】才执行一次，
//   所以第二个测试函数开始时，第一个留下的 summary.json 还在，
//   copy 会直接返回 false（表现为"装语料失败"这种莫名其妙的报错）。
bool installSummaryFixture()
{
    QFile::remove(summaryPath());
    return QFile::copy(Corpus::summaryClean(), summaryPath());
}

}  // namespace

// ★ 类名不能叫 TestOrchestrator —— 那是本项目的【产品类】（src/TestOrchestrator.h）。
//   重名会让 moc 生成的元对象张冠李戴，报出一屏和真实问题毫不相干的
//   "使用未定义类型" 错误（已实测踩过）。
class TestOrchestratorWiring : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void exportAfterCleanRun();
    void exportAfterKillDoesNotReuseStaleSummary();

private:
    // 找一个可用的 MainWindow 并填好本次输入；返回是否三个控件都找到
    static bool prepareWindow(MainWindow *mw);

    // 驱动编排层的私有槽，模拟"引擎结束了"
    static bool invokeFinished(MainWindow *mw, int exitCode, bool cleanExit);

    static QString tableRequests(MainWindow *mw);

    QStringList m_mdBefore;      // 进入测试前，reports/ 里已有的 .md
    QStringList m_created;       // 本次测试自己写出来的 .md
    bool        m_dirExisted = false;
    bool        m_summaryBackedUp = false;
};

// ---------------------------------------------------------------------------
// 记账 / 复原
// ---------------------------------------------------------------------------

void TestOrchestratorWiring::initTestCase()
{
    m_dirExisted = QDir(reportsDir()).exists();

    // summary.json 可能已经躺在那儿（上一轮遗留）。挪开，跑完再放回去 ——
    // 这个测试要自己往那儿放语料。
    if (QFile::exists(summaryPath())) {
        QFile::remove(summaryPath() + QStringLiteral(".testsave"));
        m_summaryBackedUp = QFile::rename(summaryPath(),
                                          summaryPath() + QStringLiteral(".testsave"));
        if (!m_summaryBackedUp) QFile::remove(summaryPath());
    }

    QVERIFY2(QDir().mkpath(reportsDir()), qPrintable(reportsDir()));
    m_mdBefore = snapMd(reportsDir());
}

void TestOrchestratorWiring::cleanupTestCase()
{
    // 只删【本次自己创建】的 .md —— 进测试前就在那儿的原样不动
    const QStringList after = snapMd(reportsDir());
    for (const QString &s : after) {
        const QString path = s.section(QLatin1Char('|'), 0, 0);
        if (!m_mdBefore.contains(s) && QFile::exists(path)) QFile::remove(path);
    }

    QFile::remove(summaryPath());
    if (m_summaryBackedUp) {
        QFile::rename(summaryPath() + QStringLiteral(".testsave"), summaryPath());
    } else if (!m_dirExisted) {
        QDir(reportsDir()).rmdir(reportsDir());   // 只删空目录，删不掉就算了
    }
}

// ---------------------------------------------------------------------------
// 公共前戏
// ---------------------------------------------------------------------------

bool TestOrchestratorWiring::prepareWindow(MainWindow *mw)
{
    // --- 找控件 ---
    QPushButton *exportBtn = nullptr;
    for (QPushButton *b : mw->findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("导出"))) { exportBtn = b; break; }
    }
    if (!exportBtn) return false;

    // 导出按钮初始必须是 disabled（没跑过不许导出）
    if (exportBtn->isEnabled()) return false;

    const QList<QLineEdit *> edits = mw->findChildren<QLineEdit *>();
    const QList<QSpinBox *>  spins = mw->findChildren<QSpinBox *>();
    const QList<QComboBox *> combos = mw->findChildren<QComboBox *>();
    if (edits.isEmpty() || spins.isEmpty() || combos.isEmpty()) return false;

    QLineEdit *urlEdit  = edits.first();
    QSpinBox  *vusSpin  = spins.first();
    QComboBox *durCombo = combos.first();

    // --- 填入本次输入 ---
    vusSpin->setValue(kUniqueVus);
    for (int i = 0; i < durCombo->count(); ++i) {
        if (durCombo->itemData(i).toString() == kUniqueDur) durCombo->setCurrentIndex(i);
    }

    // ★ URL 置空 → 走 preflight 的早退路径，一行拉起进程的代码都不执行
    urlEdit->setText(QString());

    // --- 点「开始压测」 ---
    QPushButton *startBtn = nullptr;
    for (QPushButton *b : mw->findChildren<QPushButton *>()) {
        if (b->text().contains(QStringLiteral("开始"))) { startBtn = b; break; }
    }
    if (!startBtn) return false;
    startBtn->click();
    QCoreApplication::processEvents();

    return true;
}

bool TestOrchestratorWiring::invokeFinished(MainWindow *mw, int exitCode, bool cleanExit)
{
    TestOrchestrator *orch = mw->findChild<TestOrchestrator *>();
    if (!orch) return false;

    const bool ok = QMetaObject::invokeMethod(
        orch, "onEngineFinished", Qt::DirectConnection,
        Q_ARG(int, exitCode), Q_ARG(bool, cleanExit), Q_ARG(QString, summaryPath()));
    QCoreApplication::processEvents();
    return ok;
}

QString TestOrchestratorWiring::tableRequests(MainWindow *mw)
{
    if (QTableWidget *tbl = mw->findChild<QTableWidget *>())
        if (QTableWidgetItem *it = tbl->item(0, 1)) return it->text();
    return QStringLiteral("<无>");
}

// ---------------------------------------------------------------------------
// D：正常跑完 → 导出
// ---------------------------------------------------------------------------

void TestOrchestratorWiring::exportAfterCleanRun()
{
    MainWindow mw;
    mw.resize(1100, 900);
    mw.show();
    QCoreApplication::processEvents();

    QVERIFY2(prepareWindow(&mw), "控件没找齐或导出按钮初始就是可点的");

    // 装语料，模拟"k6 正常跑完，summary.json 就在那儿"
    QVERIFY2(installSummaryFixture(),
             qPrintable(QStringLiteral("无法把语料放到 %1").arg(summaryPath())));
    QVERIFY(invokeFinished(&mw, 0, /*cleanExit=*/true));

    const QStringList before = snapMd(reportsDir());
    QVERIFY2(QMetaObject::invokeMethod(&mw, "onExportClicked", Qt::DirectConnection),
             "onExportClicked 不在元对象里？");
    QCoreApplication::processEvents();

    const QString produced = producedSince(before, snapMd(reportsDir()));
    QVERIFY2(!produced.isEmpty(), "导出没有产出任何文件");

    // 文件名带 14 位时间戳 = startedAt 真的接上了
    QVERIFY2(QRegularExpression(QStringLiteral("\\d{8}-\\d{6}\\.md$"))
                 .match(produced).hasMatch(),
             qPrintable(QFileInfo(produced).fileName()));

    const QString md = readAll(produced);
    QVERIFY2(!md.isEmpty(), qPrintable(produced));

    // ★ 主判据挑【有默认值】的字段：vus 默认 10、spin 初值 1，本次输入 7。
    //   "报告里的值 == 本次输入"是不可伪造的；"非空"会被默认值骗过。
    QCOMPARE(MarkdownFields::value(md, QStringLiteral("vus")),      QString::number(kUniqueVus));
    QCOMPARE(MarkdownFields::value(md, QStringLiteral("duration")), kUniqueDur);

    // 无默认值可骗的字段，只能证明"这段代码跑过了" —— 作辅证
    QVERIFY2(!MarkdownFields::value(md, QStringLiteral("脚本路径")).isEmpty()
                 && MarkdownFields::value(md, QStringLiteral("脚本路径")) != QStringLiteral("<缺失>"),
             qPrintable(md.left(400)));
    QVERIFY2(MarkdownFields::has(md, QStringLiteral("开始时间"))
                 && MarkdownFields::value(md, QStringLiteral("开始时间")) != QStringLiteral("<缺失>"),
             qPrintable(md.left(400)));

    // ★ 抓"整行都没写出来"：URL 输入为空时，报告里那一行必须【存在】且值为空
    QVERIFY2(MarkdownFields::has(md, QStringLiteral("目标 URL")),
             "报告里必须有「目标 URL」这一行");
    QVERIFY2(MarkdownFields::value(md, QStringLiteral("目标 URL")).isEmpty(),
             qPrintable(QStringLiteral("目标 URL = \"%1\"")
                            .arg(MarkdownFields::value(md, QStringLiteral("目标 URL")))));

    // 数据来自真实解析链路（语料里 http_reqs.count = 4375）
    QCOMPARE(MarkdownFields::value(md, QStringLiteral("总请求数")), QStringLiteral("4375"));

    // ★ 三源对账：报告 == 汇总表格 == summary.json
    QCOMPARE(tableRequests(&mw), QStringLiteral("4375"));
}

// ---------------------------------------------------------------------------
// E：中途强杀 → 导出（陈旧 summary.json 陷阱）
// ---------------------------------------------------------------------------

void TestOrchestratorWiring::exportAfterKillDoesNotReuseStaleSummary()
{
    MainWindow mw;
    mw.resize(1100, 900);
    mw.show();
    QCoreApplication::processEvents();

    QVERIFY2(prepareWindow(&mw), "控件没找齐或导出按钮初始就是可点的");

    // ★ 此刻 reports/summary.json 里躺着一份【上一轮的】语料（4375）。
    //   强杀之后再导出：报告里绝不能出现这个数 —— 那是陈旧文件，不是本次结果。
    //   判据是【退出状态】而不是"文件在不在"：强杀之后那个文件多半是陈旧的、
    //   不是不存在的。
    QVERIFY2(installSummaryFixture(), "装语料失败");

    QVERIFY(invokeFinished(&mw, 1, /*cleanExit=*/false));

    const QStringList before = snapMd(reportsDir());
    QVERIFY2(QMetaObject::invokeMethod(&mw, "onExportClicked", Qt::DirectConnection),
             "onExportClicked 不在元对象里？");
    QCoreApplication::processEvents();

    const QString produced = producedSince(before, snapMd(reportsDir()));
    QVERIFY2(!produced.isEmpty(), "中途强杀后应当仍能导出报告");

    const QString md = readAll(produced);

    QVERIFY2(MarkdownFields::value(md, QStringLiteral("总请求数")) != QStringLiteral("4375"),
             qPrintable(QStringLiteral("报告总请求数 = \"%1\"，沿用了陈旧的 summary.json")
                            .arg(MarkdownFields::value(md, QStringLiteral("总请求数")))));

    // 实时兜底拿不到耗时分布 → 必须写 "—"，不能编一个 0.00 ms 出来
    const QString avg = MarkdownFields::value(md, QStringLiteral("平均耗时"));
    const QString p95 = MarkdownFields::value(md, QStringLiteral("95% 耗时"));
    QVERIFY2(avg.startsWith(QStringLiteral("—")),
             qPrintable(QStringLiteral("平均耗时 = \"%1\"").arg(avg)));
    QVERIFY2(p95.startsWith(QStringLiteral("—")),
             qPrintable(QStringLiteral("95%% 耗时 = \"%1\"").arg(p95)));

    // 除零守卫：0 个请求时错误率不能是 nan / inf
    const QString err = MarkdownFields::value(md, QStringLiteral("错误率"));
    QVERIFY2(!err.contains(QStringLiteral("nan"), Qt::CaseInsensitive)
                 && !err.contains(QStringLiteral("inf"), Qt::CaseInsensitive),
             qPrintable(QStringLiteral("错误率 = \"%1\"").arg(err)));

    // ★ 表格与报告必须同源（唯一取值入口）
    QCOMPARE(tableRequests(&mw), MarkdownFields::value(md, QStringLiteral("总请求数")));
}

QTEST_MAIN(TestOrchestratorWiring)
#include "tst_orchestrator.moc"
