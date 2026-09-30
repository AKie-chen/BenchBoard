// 架构验证：主线程 + QProcess 异步读 + 轻量行扫描，能否扛住 k6 火管？
// 关键指标 = 单次 readyRead 回调的最大耗时（决定 UI 会不会卡）
#include <QCoreApplication>
#include <QProcess>
#include <QElapsedTimer>
#include <QByteArray>
#include <QStringList>
#include <QDebug>
#include <cstdio>

static QByteArray g_pending;        // 跨回调的半行残留
static qint64 g_lines       = 0;
static qint64 g_bytes       = 0;
static qint64 g_hits        = 0;    // 命中 http_req_duration 的行
static qint64 g_invocations = 0;    // readyRead 回调触发次数
static qint64 g_nsInHandler = 0;    // 回调内累计耗时
static qint64 g_maxHandlerNs = 0;   // 单次回调最大耗时 <<< 核心指标
static double g_sumDuration = 0.0;

static const char kDurKey[] = "\"metric\":\"http_req_duration\"";
static const char kValKey[] = "\"value\":";

static void handleChunk(const QByteArray &chunk)
{
    QElapsedTimer t;
    t.start();
    ++g_invocations;
    g_bytes += chunk.size();

    // 拼接残留 + 本次数据，按 '\n' 切行，末尾不足一行留作下次残留
    QByteArray buf = g_pending;
    buf.append(chunk);

    int start = 0;
    const char *p = buf.constData();
    const int n = buf.size();
    for (int i = 0; i < n; ++i) {
        if (p[i] != '\n')
            continue;
        const QByteArray line = QByteArray::fromRawData(p + start, i - start);
        start = i + 1;
        if (line.isEmpty())
            continue;
        ++g_lines;

        // === 轻量扫描：只做 indexOf，不构造 JSON 对象树 ===
        if (line.indexOf(kDurKey) >= 0) {
            ++g_hits;
            const int v = line.indexOf(kValKey);
            if (v >= 0)
                g_sumDuration += atof(p + v + 8);
        }
    }
    g_pending = buf.mid(start);

    const qint64 ns = t.nsecsElapsed();
    g_nsInHandler += ns;
    if (ns > g_maxHandlerNs)
        g_maxHandlerNs = ns;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString k6    = QString::fromLocal8Bit(argv[1]);
    const QString dir   = QString::fromLocal8Bit(argv[2]);
    const QString label = QString::fromLocal8Bit(argc > 3 ? argv[3] : "run");

    QProcess p;
    p.setProgram(k6);
    p.setWorkingDirectory(dir);
    p.setArguments({ "run", "--quiet", "-o", "json=-", "script.js" });
    p.setProcessChannelMode(QProcess::SeparateChannels);

    static qint64 errBytes = 0;

    QObject::connect(&p, &QProcess::readyReadStandardOutput, [&p]() {
        handleChunk(p.readAllStandardOutput());
    });
    QObject::connect(&p, &QProcess::readyReadStandardError, [&p]() {
        errBytes += p.readAllStandardError().size();   // 生产环境要展示，这里只计数
    });
    QObject::connect(&p, &QProcess::errorOccurred, [](QProcess::ProcessError e) {
        qCritical() << "QProcess error:" << e;
    });

    QElapsedTimer wall;
    QObject::connect(&p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                     [&](int code, QProcess::ExitStatus st) {
        const qint64 ms = wall.elapsed();

        std::printf("\n===== [%s] 结果 =====\n", qPrintable(label));
        std::printf("退出码            : %d (status=%d)\n", code, int(st));
        std::printf("墙钟时长          : %lld ms\n", (long long)ms);
        std::printf("解析行数          : %lld\n", (long long)g_lines);
        std::printf("接收字节          : %lld  (%.2f MB/s)\n",
                    (long long)g_bytes, double(g_bytes) / 1048576.0 / (double(ms) / 1000.0));
        std::printf("行速率            : %.0f 行/s\n", double(g_lines) * 1000.0 / double(ms));
        std::printf("readyRead 回调次数: %lld  (平均 %.1f 行/次)\n",
                    (long long)g_invocations,
                    g_invocations ? double(g_lines) / double(g_invocations) : 0.0);
        std::printf("回调累计耗时      : %.1f ms  (占墙钟 %.2f%%)\n",
                    double(g_nsInHandler) / 1e6,
                    100.0 * double(g_nsInHandler) / 1e6 / double(ms));
        std::printf(">>> 单次回调最大耗时: %.2f ms   <<<\n", double(g_maxHandlerNs) / 1e6);
        std::printf("命中 duration 行  : %lld  (均值 %.3f)\n",
                    (long long)g_hits, g_hits ? g_sumDuration / double(g_hits) : 0.0);
        std::printf("半行残留未处理    : %d 字节\n", int(g_pending.size()));
        std::printf("stderr 字节       : %lld\n", (long long)errBytes);
        std::fflush(stdout);
        app.quit();
    });

    wall.start();
    p.start();
    if (!p.waitForStarted(5000)) {
        std::printf("k6 启动失败\n");
        return 2;
    }
    return app.exec();
}
