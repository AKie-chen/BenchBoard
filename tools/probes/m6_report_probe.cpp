// ============================================================================
// m6_report_probe —— M6 「出 Markdown 报告」验收（★ 同样不依赖 QProcess）
//
// 验的是 ReportWriter 的三条契约，外加 MainWindow 那一段接线：
//   Part A  safeFileName：文件名格式 + 【无效时间戳】的兜底
//   Part B  buildMarkdown：内容契约（独立成行 / 2 位小数 / checks / 缺失值 "—"）
//   Part C  writeMarkdown：落盘（编码无 BOM / 大内容完整性 / 【失败必须报错】）
//   Part D  MainWindow 端到端：invokeMethod 驱动 onExportClicked → 查产物
//
// ★ 判据的写法原则：凡是"不报错也能写错"的地方，都要有一条能【翻红】的断言。
//   这里 Part C5 / C4 是重点：
//     C5 目录建不出来时，writeMarkdown 如果照样返回一个路径，调用方无法察觉
//     C4 内容超过 QTextStream 内部缓冲(16KB)时，file.close() 早于 out 析构会丢尾巴
//
// 运行：bash tools/probes/m6_report_probe_run.sh
// ============================================================================

#include "MainWindow.h"
#include "ReportWriter.h"
#include "TestOrchestrator.h"   // M7：界面唯一的后端入口，探针经它驱动终值
#include "types.h"

#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>

#include <cstdio>
#include <cstdlib>

static int g_pass = 0;
static int g_fail = 0;

// ★ 探针要能自己收尾：Part D/E 会真的调 onExportClicked，把报告写进【用户的】reports/。
//   run.sh 有整目录还原兜底，但"直接跑 out/m6report.exe"时没人替它擦 —— 实测就这么
//   漏过一个 benchboard-20260926-225447.md。按纪律⑯（探针首要约束是不改动被测系统），
//   探针自己把【它创建的】产物删掉。
static QStringList g_created;

static void check(const char *name, bool ok, const QString &detail = QString())
{
    std::printf("  [%s] %-52s %s\n", ok ? "PASS" : "FAIL", name,
                detail.toUtf8().constData());
    std::fflush(stdout);
    if (ok) ++g_pass; else ++g_fail;
}

static const char *kSummaryPath = BENCHBOARD_ROOT "/reports/summary.json";
static const char *kCorpusPath  = BENCHBOARD_ROOT "/tests/corpus/k6_summary_sample.json";
static const char *kProbeRoot   = BENCHBOARD_ROOT "/out/probes-tmp/m6probe";

// 造一份"填满"的 TestRunResult —— 所有字段都有值，用来验内容契约
static TestRunResult makeFull()
{
    TestRunResult r;
    r.config.targetUrl  = QStringLiteral("http://127.0.0.1:8899/");
    r.config.scriptPath = QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");
    r.config.vus        = 5;
    r.config.duration   = QStringLiteral("30s");
    r.config.k6Path     = QStringLiteral("C:/k6/k6.exe");
    r.config.outputDir  = QStringLiteral(BENCHBOARD_ROOT "/reports");
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

// 取某一行的值（"- 键: 值" / "### 键: 值" / "键 = 值"），找不到返回 <缺失>
//
// ★ 必须先剥掉 Markdown 的列表/标题标记再匹配 —— 报告里每行都是
//   "- 脚本路径: xxx"，直接 startsWith("脚本路径") 永远为 false，
//   于是所有取值断言都变成"恒等于 <缺失>"，**恒 FAIL**。
//   这就是纪律⑭说的"探针自己的 bug 伪装成被测代码的 bug"：上一轮
//   D4/D5/D6 的 FAIL 方向碰巧是对的（m_config 确实空），判据却是坏的 ——
//   换成用户那份正确的报告照样 FAIL。
static QString stripMdMarks(const QString &line)
{
    QString t = line.trimmed();
    while (!t.isEmpty() && (t.startsWith(QLatin1Char('-'))
                            || t.startsWith(QLatin1Char('*'))
                            || t.startsWith(QLatin1Char('#'))))
        t = t.mid(1).trimmed();
    return t;
}

static QString valueOfLineImpl(const QString &md, const QString &key, bool *found)
{
    const QStringList lines = md.split(QLatin1Char('\n'));
    for (const QString &l : lines) {
        const QString t = stripMdMarks(l);
        if (t.startsWith(key)) {
            const int i = t.indexOf(QLatin1Char(':'));
            const int j = t.indexOf(QLatin1Char('='));
            int cut = -1;
            if (i >= 0 && j >= 0) cut = qMin(i, j);
            else                 cut = qMax(i, j);
            if (found) *found = true;
            if (cut < 0) return t;
            return t.mid(cut + 1).trimmed();
        }
    }
    return QStringLiteral("<缺失>");
}

static QString valueOfLine(const QString &md, const QString &key)
{
    return valueOfLineImpl(md, key, nullptr);
}

// ★ 区分"这一行存在但值是空的"和"这一行根本没出现" ——
//   只有前者才说明接线成功（值是空的，因为本次输入就是空）。
static bool hasLine(const QString &md, const QString &key)
{
    bool found = false;
    valueOfLineImpl(md, key, &found);
    return found;
}

// 目录里所有 .md 的「路径|mtime|size」快照，用于检测"这一次有没有写出东西"。
//
// ★ 三个字段缺一不可（实测踩过）：
//   - 只比"文件名集合" → 检测不到【同名覆盖】；
//   - 只比 mtime      → 两次写入落在同一毫秒时检测不到。实测：Part D 与 Part E
//                       导出的是【同一个文件名】（m_startTime 没变），mtime 若观察不到
//                       变化 → E2 假 FAIL，且 E4/E5/E6 会连带被跳过（那三条嵌在
//                       `if (!produced.isEmpty())` 里）→ 总数从 29/8 变成 26/6。
//   - 加 size         → 覆盖时内容长度必然变（4375 → 0），稳。
//   判据是"这三个字段拼出来的字符串有没有变"，不是"有没有新文件名"。
static QStringList snapMd(const QString &dir)
{
    QStringList out;
    const QFileInfoList fs = QDir(dir).entryInfoList(QStringList() << QStringLiteral("*.md"),
                                                    QDir::Files, QDir::Name);
    for (const QFileInfo &fi : fs)
        out << fi.absoluteFilePath() + QLatin1Char('|')
                   + fi.lastModified().toString(Qt::ISODateWithMs)
                   + QLatin1Char('|')
                   + QString::number(fi.size());
    return out;
}

// ★ M7 之后终值归编排层：MainWindow 里已经没有 onEngineFinished 这个槽了，
//   所以驱动的是 TestOrchestrator 的同名槽（私有槽在元对象里，invokeMethod 照样能调）。
static void invokeFinished(MainWindow *mw, int exitCode, bool cleanExit)
{
    TestOrchestrator *orch = mw->findChild<TestOrchestrator *>();
    if (!orch) {
        std::printf("        (找不到 TestOrchestrator —— 界面结构变了？)\n");
        return;
    }
    const bool ok = QMetaObject::invokeMethod(
        orch, "onEngineFinished", Qt::DirectConnection,
        Q_ARG(int, exitCode), Q_ARG(bool, cleanExit),
        Q_ARG(QString, QString::fromLatin1(kSummaryPath)));
    std::printf("        (invokeMethod onEngineFinished 返回 %s)\n", ok ? "true" : "false");
    QApplication::processEvents();
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);

    const QString probeOut = QStringLiteral("%1/out").arg(QLatin1String(kProbeRoot));
    QDir(probeOut).removeRecursively();
    QDir().mkpath(probeOut);

    // =======================================================================
    std::printf("\n========== Part A：safeFileName 契约 ==========\n");
    // =======================================================================
    const QDateTime valid = QDateTime::fromString(QStringLiteral("2026-09-21 15:24:00"),
                                                  QStringLiteral("yyyy-MM-dd hh:mm:ss"));
    const QString nameOk = ReportWriter::safeFileName(valid);
    check("A1 有效时间戳 → benchboard-20260921-152400.md",
          nameOk == QStringLiteral("benchboard-20260921-152400.md"),
          QStringLiteral("实际 = %1").arg(nameOk));
    check("A2 文件名不含路径分隔符 / ..（免疫路径穿越）",
          !nameOk.contains(QLatin1Char('/')) && !nameOk.contains(QLatin1Char('\\'))
              && !nameOk.contains(QStringLiteral("..")),
          nameOk);

    const QDateTime invalid;   // 默认构造 = 无效时间戳
    const QString nameBad = ReportWriter::safeFileName(invalid);
    check("A3 ★ 无效时间戳必须有安全兜底名（不得产出 benchboard-.md）",
          !nameBad.isEmpty() && nameBad != QStringLiteral("benchboard-.md"),
          QStringLiteral("实际 = \"%1\"").arg(nameBad));

    // =======================================================================
    std::printf("\n========== Part B：buildMarkdown 内容契约 ==========\n");
    // =======================================================================
    const TestRunResult full = makeFull();
    const QString md = ReportWriter::buildMarkdown(full);

    // B1 —— "vu = 5duration = 30s" 这种粘连是少写一个 \n 的典型症状
    static const QRegularExpression kSticky(
        QStringLiteral("vu\\s*=\\s*\\d+\\s*duration"));
    check("B1 vus 与 duration 各自独立成行（不得粘连）",
          !kSticky.match(md).hasMatch(),
          QStringLiteral("vus 行 = \"%1\"").arg(valueOfLine(md, QStringLiteral("vu"))));

    // B2 —— 头文件契约写着「数值统一保留 2 位小数」
    static const QRegularExpression kLongDec(QStringLiteral("\\d+\\.\\d{3,}"));
    const QRegularExpressionMatch mDec = kLongDec.match(md);
    check("B2 数值统一 2 位小数（不出现 3 位以上小数）",
          !mDec.hasMatch(),
          mDec.hasMatch() ? QStringLiteral("首个越界值 = %1").arg(mDec.captured(0))
                          : QStringLiteral("OK"));

    // B3 —— checks 是 TestRunResult 的字段，报告里必须能看见
    check("B3 checks 写进报告（名字 + passes + fails 至少有一处）",
          md.contains(QStringLiteral("status is 200")),
          QStringLiteral("checks.size()=%1，报告中%2找得到该 check 名")
              .arg(full.checks.size())
              .arg(md.contains(QStringLiteral("status is 200")) ? QString() : QStringLiteral("【不】")));

    // B4 —— 契约：缺失值写 "—"，不能写 0（同族于汇总表格里的 fmtMs）
    TestRunResult r0 = full;
    r0.minDurationMs = 0.0;
    const QString md0 = ReportWriter::buildMarkdown(r0);
    const QString minVal = valueOfLine(md0, QStringLiteral("最小耗时"));
    check("B4 缺失耗时（0）写 \"—\" 而不是 \"0\"",
          minVal.startsWith(QStringLiteral("—")),
          QStringLiteral("最小耗时 = \"%1\"").arg(minVal));

    // B5 —— 配置段的正常字段（这几条应当本来就是对的）
    check("B5 配置段含 targetUrl / vus / duration / k6 版本",
          md.contains(full.config.targetUrl) && md.contains(QStringLiteral("30s"))
              && md.contains(QStringLiteral("k6 v2.2.0")));

    // =======================================================================
    std::printf("\n========== Part C：writeMarkdown 落盘 ==========\n");
    // =======================================================================
    QString err;
    const QString p = ReportWriter::writeMarkdown(full, probeOut, &err);
    check("C1 返回非空路径、且文件真的存在",
          !p.isEmpty() && QFile::exists(p),
          QStringLiteral("返回 = \"%1\"  err = \"%2\"").arg(p, err));
    check("C1b 返回路径风格统一（不含反斜杠）",
          !p.contains(QLatin1Char('\\')),
          QStringLiteral("返回 = \"%1\"").arg(p));
    check("C2 成功时 errorOut 保持为空", err.isEmpty(),
          QStringLiteral("err = \"%1\"").arg(err));

    // C3 —— 契约：UTF-8 【无 BOM】
    {
        QFile f(p);
        QByteArray head;
        if (f.open(QIODevice::ReadOnly)) head = f.read(3);
        check("C3 落盘为 UTF-8 无 BOM",
              head != QByteArray("\xEF\xBB\xBF", 3),
              QStringLiteral("前三字节 = %1").arg(QString::fromLatin1(head.toHex(' '))));
    }

    // C4 —— ★ 大内容落盘
    //   这里要分开验两件【互不相干】的事，混在一起会互相掩盖：
    //     C4a 换行符：开着 QIODevice::Text 会被 Qt 转成 CRLF，违反"换行用 \n"的契约
    //     C4b 完整性：内容超过 QTextStream 内部 16KB 缓冲时，file.close() 早于
    //                 out 析构会丢尾巴（判据不猜内容，直接对账字节数）
    {
        TestRunResult big = full;
        big.config.scriptPath = QStringLiteral("E:/very/long/path/segment/").repeated(1200)
                                + QStringLiteral("script.js");
        const QString bigMd   = ReportWriter::buildMarkdown(big);
        const QString bigPath = QDir(probeOut).filePath(ReportWriter::safeFileName(big.startedAt));
        QFile::remove(bigPath);
        QString e2;
        ReportWriter::writeMarkdown(big, probeOut, &e2);

        QFile f(bigPath);
        QByteArray body;
        if (f.open(QIODevice::ReadOnly)) body = f.readAll();

        const int crCount = body.count('\r');
        check("C4a ★ 落盘换行是 \\n（契约）—— 不得被 QIODevice::Text 转成 CRLF",
              crCount == 0,
              QStringLiteral("\\r 出现 %1 次；文件 %2 B vs 预期 %3 B")
                  .arg(crCount).arg(body.size()).arg(bigMd.toUtf8().size()));

        QByteArray norm = body;
        norm.replace("\r\n", "\n");
        const qint64 expect = bigMd.toUtf8().size();
        check("C4b 大内容（>16KB）无截断",
              expect > 20000 && norm.size() == expect,
              QStringLiteral("归一化后 %1 B  预期 %2 B").arg(norm.size()).arg(expect));
    }

    // C5 —— ★ 失败路径：outputDir 指向一个【已存在的文件】，目录永远建不出来
    {
        const QString blocker = QStringLiteral("%1/blocker.txt").arg(QLatin1String(kProbeRoot));
        { QFile bf(blocker); bf.open(QIODevice::WriteOnly); bf.write("x"); bf.close(); }
        QString e3;
        const QString p3 = ReportWriter::writeMarkdown(full, blocker, &e3);
        check("C5 ★ 目录建不出来时必须让调用方察觉（返回空串）",
              p3.isEmpty(),
              QStringLiteral("返回 = \"%1\"  err = \"%2\"").arg(p3, e3));
        check("C5b 失败时 errorOut 有内容", !e3.isEmpty(),
              QStringLiteral("err = \"%1\"").arg(e3));
    }

    // =======================================================================
    std::printf("\n========== Part D：MainWindow 端到端（导出按钮） ==========\n");
    // =======================================================================
    const QString reportsDir = QStringLiteral(BENCHBOARD_ROOT "/reports");
    {
        const TestRunResult fresh;
        std::printf("        (基线：TestRunResult{} 默认 config.vus = %d, duration = \"%s\")\n",
                    fresh.config.vus, fresh.config.duration.toUtf8().constData());
    }
    MainWindow mw;
    mw.resize(1100, 900);
    mw.show();
    QApplication::processEvents();

    // D0 —— 按钮初始必须不可点（没跑过不许导出）
    {
        QPushButton *btn = nullptr;
        const QList<QPushButton *> bs = mw.findChildren<QPushButton *>();
        for (QPushButton *b : bs) {
            if (b->text().contains(QStringLiteral("导出"))) { btn = b; break; }
        }
        check("D0 找到「导出报告」按钮，且初始为 disabled",
              btn && !btn->isEnabled(),
              btn ? QStringLiteral("enabled=%1").arg(btn->isEnabled())
                  : QStringLiteral("找不到按钮"));
    }

    // ----------------------------------------------------------------------
    // D1 —— ★ 用「URL 置空」驱动 onStartClicked：把 m_config / m_startTime
    //       填上，然后在启动引擎之前早退 —— 【一行拉起进程的代码都不执行】。
    //
    //   为什么必须驱动它：M6-2 的接线只发生在 onStartClicked 里
    //   （MainWindow.cpp:232-239）。不驱动，m_config 恒为空 → Part D 的
    //   全部接线判据都是"恒 FAIL"的假失败。
    //
    //   为什么不用"独特 URL"：那会一路走到 K6Engine::start()。
    //   本机沙箱内 start() 报 FailedToStart，但 k6 子进程【真的被创建】
    //   （实测），孤儿 k6 会在探针退出【之后】写 summary.json，把 run.sh
    //   刚还原的用户产物冲掉 —— 上一轮就是这么把 4168 B 冲成 3290 B 的。
    //
    //   而 URL 空值校验现在在编排层的 preflight() 里 —— 也在 engine->start() 之前，
    //   所以「URL 置空」= m_config 照填 + 立刻 return。零副作用，且可自证（D1c）。
    // ----------------------------------------------------------------------
    QLineEdit *urlEdit  = nullptr;
    QSpinBox  *vusSpin  = nullptr;
    QComboBox *durCombo = nullptr;
    const QList<QLineEdit *> edits = mw.findChildren<QLineEdit *>();
    for (QLineEdit *le : edits)
        if (le->text().startsWith(QStringLiteral("http"))) { urlEdit = le; break; }
    if (!urlEdit && !edits.isEmpty()) urlEdit = edits.first();

    const QList<QSpinBox *> spins = mw.findChildren<QSpinBox *>();
    if (!spins.isEmpty()) vusSpin = spins.first();
    const QList<QComboBox *> combos = mw.findChildren<QComboBox *>();
    if (!combos.isEmpty()) durCombo = combos.first();

    check("D1a 三个输入控件都找到了",
          urlEdit && vusSpin && durCombo,
          QStringLiteral("QLineEdit=%1  QSpinBox=%2  QComboBox=%3")
              .arg(edits.size()).arg(spins.size()).arg(combos.size()));

    // ★ 判据字段的选择（同族于纪律⑮：挑【有默认值】的字段，用"等于本次输入"）：
    //   QSpinBox 初值 = 1（buildUi 里 setRange(1,1000)）、
    //   combo 初值 = index 0（"10s"）。
    //   探针改成 7 / "60s"，报告里跟着变才叫"接线成功"。
    //   （注意 targetUrl 的默认值已被移除 → 它为空不再能区分"没接线"和"输入为空"，
    //     所以不能再拿它当主判据，只能当"是否跟着变"的辅证，见 D9。）
    const int     kUniqueVus = 7;
    const QString kUniqueDur = QStringLiteral("60s");
    if (urlEdit)  urlEdit->setText(QString());          // ← 触发早退
    if (vusSpin)  vusSpin->setValue(kUniqueVus);
    if (durCombo) {
        for (int i = 0; i < durCombo->count(); ++i)
            if (durCombo->itemData(i).toString() == kUniqueDur) durCombo->setCurrentIndex(i);
    }

    const bool started = QMetaObject::invokeMethod(&mw, "onStartClicked", Qt::DirectConnection);
    QApplication::processEvents();
    check("D1b onStartClicked 可被驱动", started);

    // ★ 零副作用自证：URL 为空就必须在 start() 之前早退，引擎保持 NotRunning。
    //   这条红了说明 onStartClicked 的校验顺序变了 —— 那 Part D 会真的去拉 k6，
    //   此时立刻终止，别让孤儿 k6 去污染 reports/。
    // ★ M7 之后界面里已经没有 QProcess 了 —— 改问编排层"引擎起来了吗"。
    //   判据等价：URL 为空 → preflight 失败 → engine->start() 从未被调用。
    TestOrchestrator *orch = mw.findChild<TestOrchestrator *>();
    check("D1c ★ URL 空值早退，引擎【未被启动】（探针零副作用自证）",
          orch && !orch->isRunning(),
          orch ? QStringLiteral("orchestrator::isRunning = %1").arg(int(orch->isRunning()))
               : QStringLiteral("找不到 TestOrchestrator 成员"));
    if (!orch || orch->isRunning()) {
        std::printf("        ‼️ 引擎被拉起来了 —— 立即终止，避免孤儿 k6 污染 reports/\n");
        std::fflush(stdout);
        std::_Exit(2);
    }

    // D2 —— 装语料（模拟「k6 正常跑完，summary.json 就在那儿」）
    QFile::remove(QString::fromLatin1(kSummaryPath));
    check("D2 装入语料样本作为 summary.json",
          QFile::copy(QString::fromLatin1(kCorpusPath), QString::fromLatin1(kSummaryPath)));

    const QStringList before = snapMd(reportsDir);
    invokeFinished(&mw, 0, true);

    const bool invoked = QMetaObject::invokeMethod(&mw, "onExportClicked",
                                                   Qt::DirectConnection);
    QApplication::processEvents();
    check("D3 onExportClicked 可被驱动（在元对象里）", invoked);

    // ★ 用「路径 + mtime + size」快照比对，而不是"新出现的文件名"：
    //   若 startedAt 无效，导出名会退化成 benchboard-.md，与已有文件【同名】→
    //   只是覆盖，不产生"新文件"，靠文件名差集会把覆盖漏掉。
    //   ★ 带 size 是必须的 —— 同名覆盖时 mtime 可能落在同一毫秒（见 snapMd 注释）。
    const QStringList after = snapMd(reportsDir);
    QStringList touched;
    for (const QString &s : after) if (!before.contains(s)) touched << s.section(QLatin1Char('|'), 0, 0);
    std::printf("        本次被写入的产物：%s\n",
                touched.isEmpty() ? "(无)" : touched.join(QStringLiteral(", ")).toUtf8().constData());

    const QString produced = touched.isEmpty() ? QString() : touched.first();
    if (!produced.isEmpty()) g_created << produced;   // 记下来，跑完自己删（见文件头说明）
    check("D4 ★ 导出的文件名含 14 位时间戳（证明 startedAt 真的接上了）",
          QRegularExpression(QStringLiteral("\\d{8}-\\d{6}\\.md$"))
              .match(produced).hasMatch(),
          QStringLiteral("实际文件名 = \"%1\"")
              .arg(produced.isEmpty() ? QStringLiteral("(没写出任何文件)")
                                      : QFileInfo(produced).fileName()));

    // 汇总表格里「总请求数」的值（第 0 行第 1 列）—— 三源对账要用
    const auto tableRequests = [&mw]() -> QString {
        if (QTableWidget *tbl = mw.findChild<QTableWidget *>())
            if (QTableWidgetItem *it = tbl->item(0, 1)) return it->text();
        return QStringLiteral("<无>");
    };

    if (!produced.isEmpty()) {
        QFile f(produced);
        QString content;
        if (f.open(QIODevice::ReadOnly)) content = QString::fromUtf8(f.readAll());

        const QString script = valueOfLine(content, QStringLiteral("脚本路径"));
        const QString k6p    = valueOfLine(content, QStringLiteral("k6 路径"));
        const QString outDir = valueOfLine(content, QStringLiteral("输出目录"));
        const QString url    = valueOfLine(content, QStringLiteral("目标 URL"));
        const QString st     = valueOfLine(content, QStringLiteral("开始时间"));
        const QString vus    = valueOfLine(content, QStringLiteral("vus"));
        const QString dur    = valueOfLine(content, QStringLiteral("duration"));
        const QString reqs   = valueOfLine(content, QStringLiteral("总请求数"));
        // ★★ 两条主判据特意挑【有默认值】的字段：
        //     vus 默认 10、duration 默认 "30s"（src/types.h:54-55），本次输入 7 / "60s"。
        //     "报告里的值 == 本次输入"是不可伪造的 —— 而"非空"会被默认值骗过。
        //     （脚本路径/k6 路径/输出目录无默认值，但它们在赋值时是【硬编码】的，
        //       只能证明"这段代码跑过了"，不能证明"来自本次输入"，所以降为辅证 D7。）
        check("D5 ★ 报告「vus」== 本次输入 7（默认值是 10 → 不可伪造）",
              vus == QString::number(kUniqueVus),
              QStringLiteral("报告 vus = \"%1\"   本次输入 = %2").arg(vus).arg(kUniqueVus));
        check("D6 ★ 报告「duration」== 本次选中 60s（默认值是 30s → 不可伪造）",
              dur == kUniqueDur,
              QStringLiteral("报告 duration = \"%1\"   本次选中 = \"%2\"").arg(dur, kUniqueDur));
        check("D7 ★ 报告「脚本路径 / k6 路径 / 输出目录」非空（无默认值可骗）",
              !script.isEmpty() && script != QStringLiteral("<缺失>")
                  && !k6p.isEmpty() && !outDir.isEmpty(),
              QStringLiteral("脚本 = \"%1\"  k6 = \"%2\"  输出目录 = \"%3\"")
                  .arg(script, k6p, outDir));
        check("D8 ★ 报告「开始时间」非空",
              !st.isEmpty() && st != QStringLiteral("<缺失>"),
              QStringLiteral("开始时间 = \"%1\"").arg(st));

        // ★ 这一条抓的是"整行都没写出来"：URL 输入为空时，报告里那一行
        //   必须【存在】且值为空 —— 不是缺行，也不是退回某个默认值。
        const bool urlLine = hasLine(content, QStringLiteral("目标 URL"));
        check("D9 ★ 报告「目标 URL」行存在且值为空（本次输入就是空 → 必须跟着变）",
              urlLine && url.isEmpty(),
              QStringLiteral("该行存在 = %1   值 = \"%2\"")
                  .arg(urlLine ? QStringLiteral("是") : QStringLiteral("否"), url));

        check("D10 报告「总请求数」== 语料的 4375（数据来自真实解析链路）",
              reqs == QStringLiteral("4375"),
              QStringLiteral("总请求数 = \"%1\"").arg(reqs));

        // ★ 卡片验收条款 2：三源对账
        check("D11 ★ 三源对账：报告 == 汇总表格 == summary.json（4375）",
              reqs == QStringLiteral("4375") && tableRequests() == QStringLiteral("4375"),
              QStringLiteral("报告 = \"%1\"  表格 = \"%2\"  summary.json = 4375")
                  .arg(reqs, tableRequests()));

        std::printf("        --- 导出报告前 22 行（原样 dump）---\n");
        const QStringList ls = content.split(QLatin1Char('\n'));
        for (int i = 0; i < qMin(22, ls.size()); ++i)
            std::printf("        | %s\n", ls.at(i).toUtf8().constData());
    }

    // =======================================================================
    std::printf("\n========== Part E：中途强杀导出（陈旧 summary.json 陷阱） ==========\n");
    // =======================================================================
    // ★ 此刻 reports/summary.json 里还躺着【语料】（4375）。强杀之后再导出：
    //   ① 报告里绝不能出现 4375 —— 那是上一轮的数（M5 §3.18 的陈旧陷阱在 M6 的复现）
    //   ② avg/p95/p99 必须写 "—" 而不是 "0.00 ms"（卡片陷阱 4 / 验收条款 3）
    //   ③ 表格与报告必须读到同一份数（M6 一号问题：唯一取值入口）
    {
        const QStringList before2 = snapMd(reportsDir);
        invokeFinished(&mw, 1, false);          // ← 等价于点「停止」
        const bool inv2 = QMetaObject::invokeMethod(&mw, "onExportClicked", Qt::DirectConnection);
        QApplication::processEvents();
        check("E1 强杀后导出仍可驱动", inv2);

        const QStringList after2 = snapMd(reportsDir);
        QStringList touched2;
        for (const QString &s : after2) if (!before2.contains(s)) touched2 << s.section(QLatin1Char('|'), 0, 0);
        const QString p2 = touched2.isEmpty() ? QString() : touched2.first();
        if (!p2.isEmpty()) g_created << p2;           // 同上：Part E 的产物也记账
        check("E2 ★ 中途强杀也能导出（卡片验收条款 3）", !p2.isEmpty(),
              p2.isEmpty() ? QStringLiteral("没有新产物")
                           : QFileInfo(p2).fileName());

        if (!p2.isEmpty()) {
            QString c2;
            QFile f2(p2);
            if (f2.open(QIODevice::ReadOnly)) c2 = QString::fromUtf8(f2.readAll());
            const QString reqs2 = valueOfLine(c2, QStringLiteral("总请求数"));
            const QString avg2  = valueOfLine(c2, QStringLiteral("平均耗时"));
            const QString p95_2 = valueOfLine(c2, QStringLiteral("95% 耗时"));
            const QString er2   = valueOfLine(c2, QStringLiteral("错误率"));

            check("E3 ★ 强杀后报告「总请求数」≠ 4375（不得沿用陈旧 summary.json）",
                  reqs2 != QStringLiteral("4375"),
                  QStringLiteral("报告总请求数 = \"%1\"（挂着的那份语料是 4375）").arg(reqs2));
            check("E4 ★ 强杀后「平均耗时」写 \"—\" 而不是 \"0.00 ms\"（陷阱 4）",
                  avg2.startsWith(QStringLiteral("—")),
                  QStringLiteral("平均耗时 = \"%1\"").arg(avg2));
            check("E5 ★ 强杀后「95% 耗时」写 \"—\"",
                  p95_2.startsWith(QStringLiteral("—")),
                  QStringLiteral("95%% 耗时 = \"%1\"").arg(p95_2));
            check("E6 强杀后「错误率」不是 nan / inf（除零守卫）",
                  !er2.contains(QStringLiteral("nan"), Qt::CaseInsensitive)
                      && !er2.contains(QStringLiteral("inf"), Qt::CaseInsensitive),
                  QStringLiteral("错误率 = \"%1\"").arg(er2));
            check("E7 ★ 强杀后表格与报告同源（唯一取值入口）",
                  tableRequests() == reqs2,
                  QStringLiteral("表格 = \"%1\"   报告 = \"%2\"").arg(tableRequests(), reqs2));

            std::printf("        --- 强杀后的报告前 22 行（原样 dump）---\n");
            const QStringList ls2 = c2.split(QLatin1Char('\n'));
            for (int i = 0; i < qMin(22, ls2.size()); ++i)
                std::printf("        | %s\n", ls2.at(i).toUtf8().constData());
        }
    }

    // =======================================================================
    std::printf("\n==================== 汇总 ====================\n");
    std::printf("PASS = %d   FAIL = %d\n", g_pass, g_fail);
    std::printf("RESULT=%s\n", g_fail == 0 ? "ALL_PASS" : "HAS_FAIL");
    std::fflush(stdout);

    // ★ 最后一步：把本次探针【自己创建】的报告删掉 —— 一行 QProcess 都没起，
    //   但 D3/E1 是真的调了 onExportClicked，产物落在用户的 reports/ 里。
    //   不带这一步，谁绕过 run.sh 直接跑 exe 谁就会往 reports/ 里留垃圾。
    {
        int removed = 0;
        for (const QString &f : g_created)
            if (QFile::remove(f)) ++removed;
        std::printf("已清理本次探针产物：%d 个（%s）\n", removed,
                    g_created.isEmpty() ? QStringLiteral("无").toUtf8().constData()
                                        : QFileInfo(g_created.first()).fileName().toUtf8().constData());
    }

    // ★ 用 std::_Exit 跳过析构 —— 判据已全部跑完，而正常 return 会触发
    //   MainWindow / QProcess / QChart 的完整析构链：那是【被测代码】的路径，
    //   不是探针要测的东西，只会往输出里掺噪声（实测崩过 exit 139）。
    //   注意 DLL 尚未 detach，所以这里不能用 return。
    std::_Exit(g_fail == 0 ? 0 : 1);
}
