// ============================================================================
// QProcess 启动诊断 v2 —— 四路对照，回答两个问题：
//   Q1: 到底是「Qt 那根启动同步命名管道」失败，还是「CreateProcess 根本起不来」？
//   Q2: 当前执行上下文里，有没有哪一种启动方式能成？
//
// 背景（docs/02 §3.22）：
//   QProcess::start() 稳定报 qWarning:
//       QProcess: CreateFile failed. (所有的管道范例都在使用中。)   <- ERROR_PIPE_BUSY(231)
//   而 errorString() 给的是【陈旧】的 GetLastError -> "pipe: 系统找不到指定的文件。"
//   即：服务端 CreateNamedPipe 成功，失败在客户端 CreateFile 去连自己那根管子。
//
// 四路对照（全部起同一个 exe）：
//   [1] 裸 CreateProcessW + 重定向到文件   —— 排除「CreateProcess 被沙箱拦」
//   [2] QProcess::start()                  —— 复现失败（对照组）
//   [3] QProcess::startDetached()          —— ★ 关键：这条路不建启动同步管道
//   [4] 逐个通道模式再跑一遍 start()        —— 确认与通道模式无关
//
// 输出全部英文，避免 MSVC 控制台代码页把中文打乱（本项目已踩过）。
// ============================================================================

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <cstdarg>
#include <cstdio>
#include <string>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static const QString kOutDir = QStringLiteral(BENCHBOARD_ROOT "/out/probes-tmp/qpdiag");

static void log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// 路径 -> UTF-16（CreateProcessW 要宽字符）
// ---------------------------------------------------------------------------
static std::wstring wpath(const QString &p)
{
    QString native = QDir::toNativeSeparators(p);
    return std::wstring(reinterpret_cast<const wchar_t *>(native.utf16()));
}

static std::wstring quote(const QString &s)
{
    std::wstring w = std::wstring(reinterpret_cast<const wchar_t *>(s.utf16()));
    return L"\"" + w + L"\"";
}

// ===========================================================================
// [1] 裸 CreateProcessW —— 只测「这个 exe 能不能被启动」，不测 Qt
// ===========================================================================
static bool rawCreateProcess(const QString &exe, const QStringList &args,
                             const QString &tag, DWORD waitMs,
                             DWORD *exitCode, QString *why)
{
    const QString outFile = kOutDir + "/" + tag + "_stdout.txt";
    const QString errFile = kOutDir + "/" + tag + "_stderr.txt";

    // ★ 必须让句柄可继承 —— 否则子进程拿到的是无效句柄，输出静默丢失
    //   （首版漏了这一步，raw 那组 stdout/stderr 都是 0 字节，看着像"k6 没输出"）
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hOut = CreateFileW(wpath(outFile).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = CreateFileW(wpath(errFile).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) {
        *why = QStringLiteral("CreateFile for redirection failed (err=%1)")
                   .arg(GetLastError());
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hErr != INVALID_HANDLE_VALUE) CloseHandle(hErr);
        return false;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    si.hStdInput = nullptr;

    std::wstring cmd = quote(exe);
    for (const QString &a : args) cmd += L" " + quote(a);
    // CreateProcessW 要求可写缓冲区
    std::wstring cmdBuf = cmd;

    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                                   TRUE /*bInheritHandles*/, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    const DWORD createErr = GetLastError();

    CloseHandle(hOut);
    CloseHandle(hErr);

    if (!ok) {
        *why = QStringLiteral("CreateProcessW failed (err=%1)").arg(createErr);
        return false;
    }

    const DWORD w = WaitForSingleObject(pi.hProcess, waitMs);
    *exitCode = 0;
    if (w == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, exitCode);
    } else {
        TerminateProcess(pi.hProcess, 0xDEAD);
        *exitCode = 0xFFFFFFFF;   // 超时未退出
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// ---------------------------------------------------------------------------
// 等待一个 pid 结束（返回 true = 已结束，false = 超时仍在跑）
// ---------------------------------------------------------------------------
static bool waitPid(DWORD pid, DWORD waitMs, DWORD *exitCode)
{
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                           FALSE, pid);
    if (!h) return false;
    const DWORD w = WaitForSingleObject(h, waitMs);
    if (w == WAIT_OBJECT_0) {
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        if (exitCode) *exitCode = code;
        CloseHandle(h);
        return true;
    }
    CloseHandle(h);
    return false;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const QString exe = (argc > 1) ? QString::fromLocal8Bit(argv[1])
                                   : QStringLiteral("C:/Program Files/k6/k6.exe");

    // 默认参数：跑 1 秒、2 个 VU、JSON 走 stdout
    QStringList args;
    if (argc > 2) {
        for (int i = 2; i < argc; ++i) args << QString::fromLocal8Bit(argv[i]);
    } else {
        args << QStringLiteral("run") << QStringLiteral("--quiet")
             << QStringLiteral("-o") << QStringLiteral("json=-")
             << QStringLiteral("--vus") << QStringLiteral("2")
             << QStringLiteral("--duration") << QStringLiteral("1s")
             << QStringLiteral("-e") << QStringLiteral("BASE_URL=http://127.0.0.1:8918/")
             << QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");
    }

    QDir().mkpath(kOutDir);

    log("================ qprocess_startdiag ================\n");
    log("env  QT_LOGGING_TO_CONSOLE   = %s\n",
        qgetenv("QT_LOGGING_TO_CONSOLE").isEmpty() ? "(unset)" : qgetenv("QT_LOGGING_TO_CONSOLE").constData());
    log("env  QT_FORCE_STDERR_LOGGING = %s\n",
        qgetenv("QT_FORCE_STDERR_LOGGING").isEmpty() ? "(unset)" : qgetenv("QT_FORCE_STDERR_LOGGING").constData());
    log("env  LSBOX_AUDIT_SHMEM       = %s\n",
        qgetenv("LSBOX_AUDIT_SHMEM").isEmpty() ? "(unset)" : qgetenv("LSBOX_AUDIT_SHMEM").constData());
    log("exe  = %s\n", exe.toLocal8Bit().constData());
    log("exists = %d\n\n", int(QFile::exists(exe)));

    // ---------------------------------------------------------------- [1]
    log("---------- [1] raw CreateProcessW ----------\n");
    {
        DWORD code = 0;
        QString why;
        const bool started = rawCreateProcess(exe, args, "raw", 30000, &code, &why);
        if (started) {
            log("  STARTED. exitCode=%lu (0x%lX) => %s\n", (unsigned long)code,
                (unsigned long)code,
                code == 0xFFFFFFFF ? "TIMEOUT(仍活着)" : "已退出");
        } else {
            log("  FAILED: %s\n", why.toLocal8Bit().constData());
        }
        // 看 k6 到底吐了多少字节出来
        for (const char *which : {"raw_stdout.txt", "raw_stderr.txt"}) {
            QFileInfo fi(kOutDir + "/" + which);
            log("  %-18s %lld 字节\n", which, (long long)(fi.exists() ? fi.size() : -1));
        }
    }
    log("\n");

    // ---------------------------------------------------------------- [2]
    log("---------- [2] QProcess::start()  SeparateChannels ----------\n");
    {
        QProcess p;
        QElapsedTimer t; t.start();
        p.setProgram(exe);
        p.setArguments(args);
        p.setProcessChannelMode(QProcess::SeparateChannels);
        p.setWorkingDirectory(QStringLiteral(BENCHBOARD_ROOT));
        p.start();
        const bool ok = p.waitForStarted(8000);
        log("  waitForStarted=%d  state=%d\n", int(ok), int(p.state()));
        if (!ok) {
            log("  errorString() = %s\n", p.errorString().toLocal8Bit().constData());
            log("  ^^ 注意：这是【陈旧 LastError】，真错误看上面 qWarning\n");
        } else {
            p.waitForFinished(30000);
            log("  finished exitCode=%d\n", p.exitCode());
        }
        log("  elapsed=%lldms\n", (long long)t.elapsed());
    }
    log("\n");

    // ---------------------------------------------------------------- [3]
    log("---------- [3] QProcess::startDetached()  <== KEY ----------\n");
    {
        QProcess p;
        p.setProgram(exe);
        p.setArguments(args);
        // detached 不能读管道，改成写文件
        p.setStandardOutputFile(kOutDir + "/detached_stdout.txt");
        p.setStandardErrorFile(kOutDir + "/detached_stderr.txt");
        p.setWorkingDirectory(QStringLiteral(BENCHBOARD_ROOT));

        const bool ok = p.startDetached();
        const qint64 pid = p.processId();
        log("  startDetached() = %d   pid=%lld\n", int(ok), (long long)pid);
        if (ok && pid > 0) {
            log("  pid 存活中？ ");
            QElapsedTimer t; t.start();
            bool ended = false;
            while (t.elapsed() < 30000) {
                QCoreApplication::processEvents();
                DWORD code = 0;
                if (waitPid(DWORD(pid), 200, &code)) {
                    log("已退出 exitCode=%lu\n", (unsigned long)code);
                    ended = true;
                    break;
                }
            }
            if (!ended) log("30 秒仍未退出（可能卡在连服务器）\n");
        } else {
            log("  errorString() = %s\n", p.errorString().toLocal8Bit().constData());
        }
        for (const char *which : {"detached_stdout.txt", "detached_stderr.txt"}) {
            QFileInfo fi(kOutDir + "/" + which);
            log("  %-23s %lld 字节\n", which, (long long)(fi.exists() ? fi.size() : -1));
        }
    }
    log("\n");

    // ---------------------------------------------------------------- [4]
    log("---------- [4] 三种通道模式再跑一遍 start() ----------\n");
    {
        const QProcess::ProcessChannelMode modes[] = {
            QProcess::SeparateChannels, QProcess::ForwardedChannels, QProcess::MergedChannels
        };
        const char *names[] = {"SeparateChannels", "ForwardedChannels", "MergedChannels"};
        for (int i = 0; i < 3; ++i) {
            QProcess p;
            p.setProgram(exe);
            p.setArguments(args);
            p.setProcessChannelMode(modes[i]);
            p.start();
            const bool ok = p.waitForStarted(8000);
            log("  %-18s waitForStarted=%d  err=%s\n", names[i], int(ok),
                ok ? "-" : p.errorString().toLocal8Bit().constData());
            if (ok) { p.kill(); p.waitForFinished(3000); }
        }
    }

    log("\n");

    // ---------------------------------------------------------------- [5]
    log("---------- [5] startDetached(static) + k6 自己写文件 ----------\n");
    log("  (不需要 Qt 重定向 -> 完全不碰任何管道，也不碰句柄继承)\n");
    {
        const QString jsOut  = kOutDir + "/sd2_metrics.json";
        const QString sumOut = kOutDir + "/sd2_summary.json";
        QFile::remove(jsOut);
        QFile::remove(sumOut);

        QStringList a2;
        a2 << QStringLiteral("run") << QStringLiteral("--quiet")
           << QStringLiteral("-o") << (QStringLiteral("json=") + jsOut)
           << QStringLiteral("--summary-export") << sumOut
           << QStringLiteral("--vus") << QStringLiteral("2")
           << QStringLiteral("--duration") << QStringLiteral("2s")
           << QStringLiteral("-e") << QStringLiteral("BASE_URL=http://127.0.0.1:8918/")
           << QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");

        qint64 pid = 0;
        const bool ok = QProcess::startDetached(exe, a2,
                                                QStringLiteral(BENCHBOARD_ROOT), &pid);
        log("  static startDetached() = %d   pid=%lld\n", int(ok), (long long)pid);
        if (ok && pid > 0) {
            DWORD code = 0;
            const bool ended = waitPid(DWORD(pid), 90000, &code);
            log("  进程结束=%d  exitCode=%lu\n", int(ended), (unsigned long)code);
        }
        const QString files[] = {jsOut, sumOut};
        for (const QString &f : files) {
            QFileInfo fi(f);
            log("  %-22s %lld 字节\n", f.mid(f.lastIndexOf('/') + 1).toLocal8Bit().constData(),
                (long long)(fi.exists() ? fi.size() : -1));
        }
    }

    log("\n================ done ================\n");
    return 0;
}
