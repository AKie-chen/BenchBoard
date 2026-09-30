// ============================================================================
// QProcess 启动诊断探针 —— 只回答一个问题：k6 到底起没起来？
//
// 为什么需要：MainWindow.cpp 只接了 readyReadStandardOutput / Error 和 finished，
// 没接 QProcess::errorOccurred。所以「启动失败」在界面上表现为
//   日志区只有一行"引擎已启动：" + 状态栏「引擎：运行中」+ 已跑秒数一直涨
// —— 和「k6 在跑但没输出」长得一模一样，肉眼分不出来。
//
// 本探针把 QProcess 的每一个状态变化都打出来。
// ============================================================================

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString program = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                       : QStringLiteral("C:/Program Files/k6/k6.exe");

    QProcess p;
    QElapsedTimer t;
    t.start();

    QObject::connect(&p, &QProcess::started, [&] {
        std::printf("t=%5lldms  [started]        pid=%lld\n", (long long)t.elapsed(),
                    (long long)p.processId());
    });
    QObject::connect(&p, &QProcess::errorOccurred, [&](QProcess::ProcessError e) {
        const char *name = "?";
        switch (e) {
        case QProcess::FailedToStart: name = "FailedToStart"; break;
        case QProcess::Crashed:       name = "Crashed";       break;
        case QProcess::Timedout:      name = "Timedout";      break;
        case QProcess::WriteError:    name = "WriteError";    break;
        case QProcess::ReadError:     name = "ReadError";     break;
        case QProcess::UnknownError:  name = "UnknownError";  break;
        }
        std::printf("t=%5lldms  [errorOccurred]  %s  (%s)\n", (long long)t.elapsed(),
                    name, qPrintable(p.errorString()));
        std::fflush(stdout);
    });
    QObject::connect(&p, &QProcess::readyReadStandardOutput, [&] {
        const QByteArray b = p.readAllStandardOutput();
        std::printf("t=%5lldms  [stdout]         %lld 字节\n", (long long)t.elapsed(),
                    (long long)b.size());
        std::fflush(stdout);
    });
    QObject::connect(&p, &QProcess::readyReadStandardError, [&] {
        const QByteArray b = p.readAllStandardError();
        std::printf("t=%5lldms  [stderr]         %lld 字节: %s\n", (long long)t.elapsed(),
                    (long long)b.size(), b.left(180).constData());
        std::fflush(stdout);
    });
    QObject::connect(&p, &QProcess::stateChanged, [&](QProcess::ProcessState s) {
        const char *name = (s == QProcess::NotRunning) ? "NotRunning"
                         : (s == QProcess::Starting)   ? "Starting" : "Running";
        std::printf("t=%5lldms  [stateChanged]   %s\n", (long long)t.elapsed(), name);
        std::fflush(stdout);
    });
    QObject::connect(&p, &QProcess::finished, [&](int code, QProcess::ExitStatus st) {
        std::printf("t=%5lldms  [finished]       exitCode=%d exitStatus=%s\n",
                    (long long)t.elapsed(), code,
                    st == QProcess::NormalExit ? "NormalExit" : "CrashExit");
        std::fflush(stdout);
        app.quit();
    });

    p.setProgram(program);
    QStringList args;
    args << QStringLiteral("run") << QStringLiteral("--quiet")
         << QStringLiteral("-o") << QStringLiteral("json=-")
         << QStringLiteral("--vus") << QStringLiteral("2")
         << QStringLiteral("--duration") << QStringLiteral("2s")
         << QStringLiteral("-e") << QStringLiteral("BASE_URL=http://127.0.0.1:8918/")
         << QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");
    p.setArguments(args);
    p.setProcessChannelMode(QProcess::SeparateChannels);

    std::printf("program = %s\n", qPrintable(program));
    std::printf("exists  = %d\n", int(QFile::exists(program)));
    std::printf("启动中...\n");
    std::fflush(stdout);
    p.start();
    p.waitForStarted(3000);
    std::printf("waitForStarted 返回后 state=%d\n",
                int(p.state()));

    QTimer::singleShot(12000, [&] {
        std::printf("12 秒兜底：还没 finished，强制收尾\n");
        p.kill();
        app.quit();
    });

    return app.exec();
}
