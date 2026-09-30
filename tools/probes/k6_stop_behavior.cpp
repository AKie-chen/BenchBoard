#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString k6 = QString::fromLocal8Bit(argv[1]);
    const QString dir = QString::fromLocal8Bit(argv[2]);

    QProcess p;
    p.setProgram(k6);
    p.setWorkingDirectory(dir);
    p.setArguments({ "run","--quiet",
                     "--summary-export","s1.json",
                     "--summary-trend-stats","avg,min,med,max,p(90),p(95),p(99)",
                     "-o","json=-", "script_long.js" });
    p.setProcessChannelMode(QProcess::SeparateChannels);

    QObject::connect(&p, &QProcess::readyReadStandardOutput,
                     [&p]{ p.readAllStandardOutput(); });
    QObject::connect(&p, &QProcess::readyReadStandardError,
                     [&p]{ p.readAllStandardError(); });

    QElapsedTimer t;
    QObject::connect(&p, QOverload<int,QProcess::ExitStatus>::of(&QProcess::finished),
        [&](int code, QProcess::ExitStatus st){
            std::printf("terminate() 后 %.2f 秒进程结束: exitCode=%d exitStatus=%d\n",
                        t.elapsed()/1000.0, code, int(st));
            QFileInfo fi(dir + "/s1.json");
            std::printf("summary s1.json 是否产出: %s (size=%lld)\n",
                        fi.exists() ? "是" : "否", (long long)fi.size());
            app.quit();
        });

    p.start();
    p.waitForStarted(5000);
    std::printf("k6 已启动，2 秒后调用 terminate()（脚本设定跑 60 秒）\n");

    t.start();
    QTimer::singleShot(2000, [&]{
        std::printf(">>> 调用 terminate()\n");
        p.terminate();
        QTimer::singleShot(3000, [&]{
            if (p.state() != QProcess::NotRunning) {
                std::printf("!!! terminate() 3 秒内未生效，转为 kill()\n");
                p.kill();
            }
        });
    });
    QTimer::singleShot(12000, [&]{ std::printf("超时兜底退出\n"); app.quit(); });

    return app.exec();
}
