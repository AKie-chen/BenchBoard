#include "K6Engine.h"

#include <QFileInfo>

namespace {

// config → summary.json 的路径推导，**唯一来源**。
//
// ★ 为什么是自由函数而不是成员：`buildArguments()` 是 `static`（为了让参数拼装
//   能脱离进程单测），拿不到 `m_summaryPath` 这类实例状态。
//   抽成一个静态自由函数之后，`buildArguments()` / `start()` 都调它 ——
//   路径只有一处推导逻辑，不会出现"写到了 A、读的是 B"。
QString summaryPathFor(const TestConfig &config)
{
    return config.outputDir + QStringLiteral("/summary.json");
}

}  // namespace

K6Engine::K6Engine(QObject *parent)
    : LoadEngine(parent)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &K6Engine::onReadyReadStdout);
    connect(&m_process, &QProcess::readyReadStandardError,  this, &K6Engine::onReadyReadStderr);
    connect(&m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &K6Engine::onProcessFinished);
    // ★ 不接这一条 = 启动失败完全静默（异常路径 1）：k6 路径写错时界面永远停在
    //   "运行中"，一句提示都没有。成本只有这一行。
    connect(&m_process, &QProcess::errorOccurred, this, &K6Engine::onProcessError);

    // stop() 之后的强杀兜底：kill() 之后进程若还在，3 秒后再补一刀。
    m_stopTimer.setSingleShot(true);
    m_stopTimer.setInterval(3000);
    connect(&m_stopTimer, &QTimer::timeout, this, &K6Engine::onStopTimeout);
}

K6Engine::~K6Engine()
{
    // 压测中关窗口（异常路径 5）：先把子进程收掉。
    // ★ 实测踩过：孤儿 k6 会在父进程退出【之后】继续跑，还能覆盖 reports/ 里的文件。
    //   所以这里不能省。
    if (m_process.state() != QProcess::NotRunning) {
        m_process.kill();
        m_process.waitForFinished(2000);
    }
}

QString K6Engine::name() const
{
    return QStringLiteral("k6");
}

bool K6Engine::isAvailable(QString *errorOut) const
{
    const QString path = detectK6Path();
    if (path.isEmpty()) {
        if (errorOut) *errorOut = QStringLiteral("未找到 k6 可执行文件，请检查安装路径。");
        return false;
    }
    if (errorOut) errorOut->clear();
    return true;
}

void K6Engine::start(const TestConfig &config)
{
    const QString path = config.k6Path.isEmpty() ? detectK6Path() : config.k6Path;
    if (path.isEmpty()) {
        emit startFailed(QStringLiteral("未找到 k6 可执行文件，请检查安装路径。"));
        return;
    }

    m_k6Path      = path;
    m_summaryPath = summaryPathFor(config);   // 和 buildArguments() 调的是同一个函数
    m_stopping    = false;

    // 绝不拼命令行字符串：URL 与脚本路径来自用户输入，拼串就是命令注入。
    // setProgram + setArguments 不走 shell，每个元素原样进一个 argv。
    m_process.setProgram(m_k6Path);
    m_process.setArguments(buildArguments(config));
    // 不分开的话 stderr 会混进 stdout，JSON 流里掺进进度条就解析不了了
    m_process.setProcessChannelMode(QProcess::SeparateChannels);

    m_wallClock.start();
    m_stopTimer.stop();

    emit started();
    // 异步启动：立刻返回，结果经 readyRead / finished / errorOccurred 到达
    m_process.start();
}

void K6Engine::stop()
{
    if (m_process.state() == QProcess::NotRunning) return;
    m_stopping = true;
    // ★ 不要写 terminate()。实测对 k6 完全无效：Windows 上 terminate() 靠给顶层窗口
    //   发 WM_CLOSE，而 k6 是无窗口控制台程序，收不到 —— 3 秒后进程还在跑。
    m_process.kill();
    m_stopTimer.start();   // 兜底
}

bool K6Engine::isRunning() const
{
    // ★ 不能写成 `return !m_stopping;` —— 进程压根没起来时 m_stopping 也是 false，
    //   会误报"在跑"（实测：未启动时 QProcess::state() == NotRunning）。
    return m_process.state() != QProcess::NotRunning;
}

QString K6Engine::detectK6Path()
{
    const QStringList candidates = {
        QStringLiteral("C:/Program Files/k6/k6.exe"),
        QStringLiteral("C:/Program Files (x86)/k6/k6.exe"),
        QStringLiteral("C:/ProgramData/chocolatey/bin/k6.exe"),
    };
    for (const QString &c : candidates)
        if (QFileInfo::exists(c)) return c;
    return QString();
}

QString K6Engine::queryVersion(const QString &k6Path)
{
    if (k6Path.isEmpty()) return QString();
    // 同步跑一次 `k6 version`：这是启动时的一次性探测，不在热路径上。
    QProcess p;
    p.start(k6Path, QStringList() << QStringLiteral("version"));
    if (!p.waitForFinished(3000)) {
        p.kill();
        p.waitForFinished(500);
        return QString();
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return QString();
    return QString::fromUtf8(p.readAllStandardOutput()).trimmed();
}

const QString &K6Engine::lastSummaryPath() const
{
    return m_summaryPath;
}

void K6Engine::onReadyReadStdout()
{
    // --- 取走数据（不取走管道会满，k6 会阻塞在 write() 上）---
    const QByteArray chunk = m_process.readAllStandardOutput();

    // 本类【不解析】：原样把字节交出去。拆行 / 半行残留 / JSON 解析都在 MetricsAggregator。
    // ★ 管道边界 ≠ 行边界 —— 但"拼半行"的活是聚合器的事（它有 m_pending），这里只搬字节。
    emit outputChunk(chunk);
}

void K6Engine::onReadyReadStderr()
{
    // 必须读：实测 8 秒能写 5.9 MB stderr，不读 → 管道缓冲区写满 → k6 阻塞在 write()
    // 上 → 表现是"压测莫名卡死"，而且报错全堵在管道里，你连原因都看不到。
    const QByteArray err = m_process.readAllStandardError();
    if (err.isEmpty()) return;

    const QStringList lines = QString::fromUtf8(err).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines)
        emit logOutput(line.trimmed());
}

void K6Engine::onProcessFinished(int exitCode, QProcess::ExitStatus status)
{
    m_stopTimer.stop();
    m_stopping = false;

    // ★ cleanExit 的唯一来源就是这里 —— 别让它烂在实现类里。
    //   status == NormalExit  → 进程自己退出的：exitCode 有语义、summary 可信
    //   status == CrashExit   → 被 kill() / 崩掉的：exitCode 无意义（实测 62097）
    emit finished(exitCode, status == QProcess::NormalExit, m_summaryPath);
}

void K6Engine::onProcessError(QProcess::ProcessError error)
{
    if (error == QProcess::FailedToStart) {
        // ★ 不要拿 errorString() 当判据 —— 它是陈旧的 GetLastError()，会骗你
        //   （docs/02 §3.22）。可执行的提示是"路径 + 发生了什么"。
        emit startFailed(QStringLiteral("无法启动 k6（%1）—— 请检查可执行文件是否存在、是否可执行")
                             .arg(m_k6Path.isEmpty() ? QStringLiteral("路径未知") : m_k6Path));
        return;
    }
    if (error == QProcess::Crashed)
        emit healthWarning(QStringLiteral("k6 进程异常崩溃（不是用户点停止）"));
}

void K6Engine::onStopTimeout()
{
    if (m_process.state() != QProcess::NotRunning) {
        emit healthWarning(QStringLiteral("k6 未在 3 秒内退出，已再次强制结束"));
        m_process.kill();
    }
}

// 组装 k6 命令行参数（供单测使用，纯函数）
QStringList K6Engine::buildArguments(const TestConfig &config)
{
    QStringList args;
    args << QStringLiteral("run")
         << QStringLiteral("--quiet")                                      // 关进度条。不关的话 stderr 刷满 ANSI 控制字符
         << QStringLiteral("-o") << QStringLiteral("json=-");              // ★ 必须 =-，写 stdout 会生成 58MB 文件
    // ★ --summary-export 收的是【文件路径】不是目录。
    //   static 方法拿不到 m_summaryPath，所以两处都调 summaryPathFor()（唯一来源）。
    args << QStringLiteral("--summary-export") << summaryPathFor(config);
    args << QStringLiteral("--summary-trend-stats")
         << QStringLiteral("avg,min,med,max,p(90),p(95),p(99),p(99.9)");   // 不加就没有 p99
    args << QStringLiteral("--vus") << QString::number(config.vus);               // 不加界面输入框就是摆设
    args << QStringLiteral("--duration") << config.duration;

    // --- 让 URL 输入框真正生效 ---
    // k6 用 -e KEY=VALUE 传环境变量，脚本里通过 __ENV.KEY 读。
    // 一次占两个元素：写成 "-e BASE_URL=..." 一整个字符串会被 k6 当成一个未知参数。
    args << QStringLiteral("-e") << (QStringLiteral("BASE_URL=") + config.targetUrl);

    args << config.scriptPath;

    return args;
}
