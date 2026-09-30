// QProcess 通道模式诊断 —— 定位"pipe: 系统找不到指定的文件"到底出在哪一步
// 依次尝试：SeparateChannels(默认) / ForwardedChannels / MergedChannels+文件
#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <cstdio>

static void flush() { std::fflush(stdout); }

static void tryRun(const char *label, QProcess::ProcessChannelMode mode, const QString &outFile)
{
    std::printf("\n========== %s ==========\n", label);
    flush();

    QProcess p;
    QObject::connect(&p, &QProcess::errorOccurred, [&](QProcess::ProcessError e) {
        std::printf("  [errorOccurred] code=%d  errorString=<%s>  (0=FailedToStart 1=Crashed 2=Timedout)\n",
                    int(e), qPrintable(p.errorString()));
        flush();
    });
    QObject::connect(&p, &QProcess::started, [&] {
        std::printf("  [started] pid=%lld\n", (long long)p.processId());
        flush();
    });

    p.setProcessChannelMode(mode);
    if (!outFile.isEmpty())
        p.setStandardOutputFile(outFile);

    p.setProgram(QStringLiteral("C:/Windows/System32/where.exe"));
    p.setArguments(QStringList{ QStringLiteral("cmd") });

    p.start();
    const bool ok = p.waitForStarted(3000);
    std::printf("  waitForStarted -> %d    state=%d    errorString=<%s>\n",
                int(ok), int(p.state()), qPrintable(p.errorString()));
    flush();

    if (ok) {
        p.waitForFinished(3000);
        std::printf("  exitCode=%d  exitStatus=%d\n", p.exitCode(), int(p.exitStatus()));
        flush();
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    tryRun("A) SeparateChannels（默认，Qt 走命名管道）",
           QProcess::SeparateChannels, QString());

    tryRun("B) ForwardedChannels（不走管道，直接继承父进程句柄）",
           QProcess::ForwardedChannels, QString());

    tryRun("C) MergedChannels + setStandardOutputFile（输出到文件）",
           QProcess::MergedChannels, QStringLiteral(BENCHBOARD_ROOT "/out/probes-tmp/qpdiag/out.txt"));

    std::printf("\n=== 诊断结束 ===\n");
    return 0;
}
