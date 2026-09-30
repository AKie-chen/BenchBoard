// ============================================================================
// 分层守卫（架构适应度函数）
//
// 这是整个测试套件里【唯一】读源码文本的地方 —— 别的测试跑代码，它读文本。
// 它守的是这条铁律：
//
//     UI 只认识 TestOrchestrator，不认识 QProcess。
//     全仓只有 K6Engine 这一个文件"认识" QProcess。
//
// 为什么值得写成自动化测试：分层是"注释里写一百遍也没人管"的那类约束。
// 将来加 vegeta 引擎、或者顺手在 MainWindow 里读一下进程状态，
// 这个测试会当场翻红 —— 而不是等到某天有人想把引擎换成 REST 时才发现改不动。
//
// ★ 判据怎么写才不骗人（实测踩过两次同类）：
//     ① `grep -c QProcess == 0` —— 注释里出现 `QProcess::` 也会被数进去
//        （TestOrchestrator.cpp 数出 4 处，【全在注释里】）→ 按字面永远过不了；
//     ② 换成找 `#include <QProcess>` 之后 —— 注释里【引用这条判据本身】的说明
//        文字（"正解是删掉 #include <QProcess> 后仍能编译"）同样被数进去 → 又失败。
//   → 结论：任何"读源码文本"的判据都会被注释污染，除非先把注释剥掉。
//
// 来源：tools/probes/m7_engine_probe.cpp 的 Part C。
// ============================================================================

#include <QtTest>

#include "TestPaths.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

namespace {

// 剥掉 `//` 行注释与 `/* */` 块注释。
//
// 字符串字面量【原样保留】（不去掉内容），但它的内部不再被当作代码：
// 所以 `"http://x"` 里的 `//` 不会误开一段注释 —— 这才是这个函数容易写错的地方。
QString stripComments(const QString &src)
{
    QString out;
    bool block = false;

    for (int i = 0; i < src.size(); ++i) {
        const QChar c = src.at(i);
        const QChar n = (i + 1 < src.size()) ? src.at(i + 1) : QChar();

        if (block) {
            if (c == QLatin1Char('*') && n == QLatin1Char('/')) { block = false; ++i; }
            else if (c == QLatin1Char('\n')) out += c;   // 保留换行，维持行结构
            continue;
        }

        if (c == QLatin1Char('/') && n == QLatin1Char('*')) { block = true; ++i; continue; }

        if (c == QLatin1Char('/') && n == QLatin1Char('/')) {
            while (i < src.size() && src.at(i) != QLatin1Char('\n')) ++i;
            out += QLatin1Char('\n');
            continue;
        }

        if (c == QLatin1Char('"')) {                      // 原样收录，内部不解释
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
bool codeContains(const QString &path, const QString &needle)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    return stripComments(QString::fromUtf8(f.readAll())).contains(needle);
}

QStringList sourceFiles()
{
    QDir dir(TestPaths::srcDir());
    return dir.entryList({QStringLiteral("*.h"), QStringLiteral("*.cpp")}, QDir::Files, QDir::Name);
}

}  // namespace

class TestLayering : public QObject
{
    Q_OBJECT

private slots:
    void sourceTreeIsPresent();
    void engineKnowsNothingAboutWidgets();
    void orchestratorDoesNotKnowAboutProcess();
    void uiDoesNotKnowAboutProcess();
    void onlyEngineKnowsAboutProcess();
};

void TestLayering::sourceTreeIsPresent()
{
    QVERIFY2(QFile::exists(TestPaths::src(QStringLiteral("K6Engine.h"))),
             qPrintable(TestPaths::srcDir()));
    QVERIFY2(sourceFiles().size() > 10,
             qPrintable(QStringLiteral("src/ 下只找到 %1 个源文件，路径可能不对")
                            .arg(sourceFiles().size())));
}

// 引擎层不该认识界面 —— 认识 QtWidgets 就说明它可以被界面细节牵着走
void TestLayering::engineKnowsNothingAboutWidgets()
{
    for (const QString &f : {QStringLiteral("K6Engine.h"), QStringLiteral("K6Engine.cpp")}) {
        const QString path = TestPaths::src(f);
        QVERIFY2(!codeContains(path, QStringLiteral("QtWidgets")), qPrintable(f));
        QVERIFY2(!codeContains(path, QStringLiteral("QMainWindow")), qPrintable(f));
    }
}

// 编排层连 <QProcess> 都不该 include —— 它只跟 LoadEngine 接口打交道。
// 这条一旦破了，"换引擎不用动上层"就成了空话。
void TestLayering::orchestratorDoesNotKnowAboutProcess()
{
    for (const QString &f : {QStringLiteral("TestOrchestrator.h"),
                             QStringLiteral("TestOrchestrator.cpp")}) {
        const QString path = TestPaths::src(f);
        QVERIFY2(!codeContains(path, QStringLiteral("#include <QProcess>")), qPrintable(f));
    }
}

void TestLayering::uiDoesNotKnowAboutProcess()
{
    for (const QString &f : {QStringLiteral("MainWindow.h"), QStringLiteral("MainWindow.cpp")}) {
        const QString path = TestPaths::src(f);
        QVERIFY2(!codeContains(path, QStringLiteral("#include <QProcess>")), qPrintable(f));
    }
}

// ★★ 比"某个文件不 include"更硬的判据：全仓【只有一个】文件认识 QProcess。
//
// 这条是分层能否长期成立的守门人：将来加 vegeta 时，新引擎实现类当然可以
// include <QProcess>，那时这里会翻红 —— 提醒你显式地更新这份白名单，
// 而不是让分层悄悄烂掉。
void TestLayering::onlyEngineKnowsAboutProcess()
{
    QStringList owners;
    for (const QString &name : sourceFiles()) {
        if (codeContains(TestPaths::src(name), QStringLiteral("#include <QProcess>")))
            owners << name;
    }

    QVERIFY2(owners == QStringList{QStringLiteral("K6Engine.h")},
             qPrintable(QStringLiteral("认识 QProcess 的文件应当只有 [K6Engine.h]，实际 = [%1]")
                            .arg(owners.isEmpty() ? QStringLiteral("(无)")
                                                  : owners.join(QStringLiteral(", ")))));
}

QTEST_GUILESS_MAIN(TestLayering)
#include "tst_layering.moc"
