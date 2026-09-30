// ============================================================================
// m7_engine_probe —— M7 分层验收（★ 不需要 QProcess / k6，纯函数 + 静态断言）
//
// 为什么能不起进程：
//   ① `buildArguments()` 是纯函数 —— 参数拼装的正确性可以在"不拉起 k6"的前提下
//      逐条断言（这正是 M7-3 把它设计成 static 的理由）。
//   ② 分层判据（谁认识 QProcess）是**静态**的：读源码文件内容断言，跟运行无关。
//
// 本机执行上下文里 QProcess::start() 起不了子进程（docs/02 §3.23 启动矩阵），
// 所以这一层能覆盖的部分，就必须真的覆盖掉 —— 不留"本该能测却推给手工"的空白。
// ============================================================================

#include "K6Engine.h"
#include "LoadEngine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QStringList>

#include <cstdio>

static int g_pass = 0;
static int g_fail = 0;

static void check(const char *name, bool ok, const QString &detail = QString())
{
    if (ok) { ++g_pass; std::printf("  [PASS] %s\n", name); }
    else {
        ++g_fail;
        std::printf("  [FAIL] %s", name);
        if (!detail.isEmpty()) std::printf("   %s", detail.toUtf8().constData());
        std::printf("\n");
    }
}

// 取 key 后面紧跟的那个元素（k6 的参数都是 `--key value` 成对出现）
static QString after(const QStringList &l, const QString &key)
{
    const int i = l.indexOf(key);
    return (i >= 0 && i + 1 < l.size()) ? l.at(i + 1) : QStringLiteral("<缺失>");
}

static bool containsWord(const QString &path, const QString &needle)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return QString::fromUtf8(f.readAll()).contains(needle);
}

// ★ 剥掉 `//` 行注释、`/* */` 块注释和字符串字面量后再比 —— 这一步不能省。
//
//   实测踩过两次同类：
//     ① `grep -c QProcess == 0`：注释里的 `QProcess::` 会被数进去
//        （`TestOrchestrator.cpp` 数出 4 处，【全在注释里】→ 判据按字面永远过不了）；
//     ② 换成找 `#include <QProcess>` 之后：注释里【引用这条判据本身】的说明文字
//        （"正解是删掉 #include <QProcess> 后仍能编译"）同样被数进去 → 又判失败。
//   → 结论：任何"读源码文本"的判据都会被注释污染，除非先把注释剥掉。
static QString stripComments(const QString &src)
{
    QString out;
    bool block = false;
    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        const QChar n = (i + 1 < src.size()) ? src.at(i + 1) : QChar();
        if (block) {
            if (c == QLatin1Char('*') && n == QLatin1Char('/')) { block = false; ++i; }
            else if (c == QLatin1Char('\n')) out += c;    // 保留换行，维持行结构
            continue;
        }
        if (c == QLatin1Char('/') && n == QLatin1Char('*')) { block = true; ++i; continue; }
        if (c == QLatin1Char('/') && n == QLatin1Char('/')) {
            while (i < src.size() && src.at(i) != QLatin1Char('\n')) ++i;
            out += QLatin1Char('\n');
            continue;
        }
        if (c == QLatin1Char('"')) {                       // 跳过字符串字面量
            out += c;
            ++i;
            while (i < src.size() && src.at(i) != QLatin1Char('"')) { out += src.at(i); ++i; }
            if (i < src.size()) out += src.at(i);
            continue;
        }
        out += c;
    }
    return out;
}

// 只看"真实代码"里有没有这个 token
static bool codeContains(const QString &path, const QString &needle)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return stripComments(QString::fromUtf8(f.readAll())).contains(needle);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    QCoreApplication app(argc, argv);

    const QString kSrc = QStringLiteral(BENCHBOARD_ROOT "/src/");

    // =======================================================================
    std::printf("\n========== Part A：buildArguments（纯函数，不起进程） ==========\n");
    // =======================================================================
    TestConfig cfg;
    cfg.targetUrl  = QStringLiteral("http://127.0.0.1:8899/");
    cfg.vus        = 7;                                        // 默认值是 10
    cfg.duration   = QStringLiteral("60s");                    // 默认值是 30s
    cfg.outputDir  = QStringLiteral(BENCHBOARD_ROOT "/reports");
    cfg.k6Path     = QStringLiteral("C:/Program Files/k6/k6.exe");
    cfg.scriptPath = QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");

    const QStringList args = K6Engine::buildArguments(cfg);
    std::printf("        args = %s\n", args.join(QStringLiteral(" ")).toUtf8().constData());

    check("A1 第一个参数是 run", args.value(0) == QStringLiteral("run"));
    check("A2 -o json=-（= 号不能丢，丢了会生成 58 MB 文件）",
          after(args, QStringLiteral("-o")) == QStringLiteral("json=-"),
          after(args, QStringLiteral("-o")));
    check("A3 ★ --vus == 本次输入 7（默认值是 10 → 不可伪造）",
          after(args, QStringLiteral("--vus")) == QStringLiteral("7"),
          after(args, QStringLiteral("--vus")));
    check("A4 ★ --duration == 本次输入 60s（默认值是 30s → 不可伪造）",
          after(args, QStringLiteral("--duration")) == QStringLiteral("60s"),
          after(args, QStringLiteral("--duration")));
    check("A5 ★ --summary-export 传的是【文件路径】而不是目录",
          after(args, QStringLiteral("--summary-export")) == cfg.outputDir + QStringLiteral("/summary.json"),
          after(args, QStringLiteral("--summary-export")));
    check("A6 --summary-trend-stats 含 p(99)（不加就没有 p99）",
          after(args, QStringLiteral("--summary-trend-stats")).contains(QStringLiteral("p(99)")),
          after(args, QStringLiteral("--summary-trend-stats")));
    check("A7 ★ -e BASE_URL== 本次输入 URL",
          after(args, QStringLiteral("-e")) == QStringLiteral("BASE_URL=") + cfg.targetUrl,
          after(args, QStringLiteral("-e")));
    check("A8 最后一个参数是脚本路径", args.last() == cfg.scriptPath, args.last());
    check("A9 参数个数 == 15（逐个消费，不是靠猜）", args.size() == 15,
          QStringLiteral("实际 = %1").arg(args.size()));
    check("A10 参数里没有空串（空元素会被 k6 当成一个未知参数）",
          !args.contains(QString()));

    // =======================================================================
    std::printf("\n========== Part B：引擎可判定状态 ==========\n");
    // =======================================================================
    K6Engine eng;
    check("B1 未启动时 isRunning() == false（★ 不能写成 !m_stopping）", !eng.isRunning());
    check("B2 name() == \"k6\"", eng.name() == QStringLiteral("k6"), eng.name());
    check("B3 detectK6Path() 能找到本机 k6", !K6Engine::detectK6Path().isEmpty(),
          QStringLiteral("实际 = \"%1\"").arg(K6Engine::detectK6Path()));
    check("B4 未 start 时 lastSummaryPath() 为空", eng.lastSummaryPath().isEmpty(),
          eng.lastSummaryPath());
    {
        QString err;
        check("B5 isAvailable() 与 detectK6Path() 结论一致",
              eng.isAvailable(&err) == !K6Engine::detectK6Path().isEmpty(), err);
    }

    // =======================================================================
    std::printf("\n========== Part C：分层静态断言（读源码） ==========\n");
    // =======================================================================
    check("C1 K6Engine 不 include 任何 Qt Widgets / QMainWindow",
          !codeContains(kSrc + QStringLiteral("K6Engine.h"), QStringLiteral("QtWidgets"))
              && !codeContains(kSrc + QStringLiteral("K6Engine.h"), QStringLiteral("QMainWindow"))
              && !codeContains(kSrc + QStringLiteral("K6Engine.cpp"), QStringLiteral("QtWidgets"))
              && !codeContains(kSrc + QStringLiteral("K6Engine.cpp"), QStringLiteral("QMainWindow")));

    check("C2 编排层不 include <QProcess>（已剥注释）",
          !codeContains(kSrc + QStringLiteral("TestOrchestrator.cpp"), QStringLiteral("#include <QProcess>"))
              && !codeContains(kSrc + QStringLiteral("TestOrchestrator.h"), QStringLiteral("#include <QProcess>")));

    check("C3 界面不 include <QProcess>（已剥注释）",
          !codeContains(kSrc + QStringLiteral("MainWindow.cpp"), QStringLiteral("#include <QProcess>"))
              && !codeContains(kSrc + QStringLiteral("MainWindow.h"), QStringLiteral("#include <QProcess>")));

    // ★ 比 grep 更硬的判据：全仓只有引擎这一个文件"认识" QProcess。
    //   为什么不用 `grep -c QProcess == 0` —— 那条按字面永远过不了：注释里出现
    //   `QProcess::` 也会被数进去（M7 实测：TestOrchestrator.cpp 数出 4，全在注释里）。
    {
        QDir dir(kSrc);
        const QStringList files = dir.entryList(
            QStringList() << QStringLiteral("*.h") << QStringLiteral("*.cpp"), QDir::Files, QDir::Name);
        QStringList owners;
        for (const QString &n : files) {
            if (codeContains(dir.filePath(n), QStringLiteral("#include <QProcess>")))
                owners << n;
        }
        check("C4 ★★ 全仓只有 K6Engine.h 认识 QProcess（分层判据）",
              owners == QStringList{QStringLiteral("K6Engine.h")},
              QStringLiteral("实际 = %1")
                  .arg(owners.isEmpty() ? QStringLiteral("(无)") : owners.join(QStringLiteral(", "))));
    }

    std::printf("\n==================== 汇总 ====================\n");
    std::printf("PASS = %d   FAIL = %d\n", g_pass, g_fail);
    std::printf("RESULT=%s\n", g_fail == 0 ? "ALL_PASS" : "HAS_FAIL");
    std::fflush(stdout);
    return g_fail == 0 ? 0 : 1;
}
