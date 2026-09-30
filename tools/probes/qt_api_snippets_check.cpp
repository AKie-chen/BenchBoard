// 验证 M2 的 TODO 注释里给出的每一段 API 用法都能编译。
// 这不是参考实现（逻辑是空的），只做「签名/重载/类型」的编译期校验。
#include <QApplication>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <cstdio>

class Probe : public QObject
{
    Q_OBJECT
public:
    Probe()
    {
        m_engine = new QProcess(this);

        m_durationCombo = new QComboBox();
        m_durationCombo->addItem(QStringLiteral("10 秒"), QStringLiteral("10s"));
        m_durationCombo->addItem(QStringLiteral("30 秒"), QStringLiteral("30s"));
        m_durationCombo->addItem(QStringLiteral("1 分钟"), QStringLiteral("60s"));

        m_logView = new QPlainTextEdit();

        // ---- 校验点 1：TODO(M2-3) 里的 4 条 connect 写法 ----
        const bool c1 = bool(connect(m_engine, &QProcess::readyReadStandardOutput,
                                     this, &Probe::onEngineStdout));
        const bool c2 = bool(connect(m_engine, &QProcess::readyReadStandardError,
                                     this, &Probe::onEngineStderr));
        const bool c3 = bool(connect(m_engine, &QProcess::finished,
                                     this, &Probe::onEngineFinished));
        std::printf("connect ok: stdout=%d stderr=%d finished=%d\n", c1, c2, c3);

        // ---- 校验点 2：TODO(M2-6) 的计数与首包样本 ----
        const QByteArray chunk = m_engine->readAllStandardOutput();
        m_rxBytes += quint64(chunk.size());
        m_rxLines += quint64(chunk.count('\n'));

        const QList<QByteArray> lines = chunk.split('\n');
        const int showN = qMin(2, int(lines.size()));
        for (int i = 0; i < showN; ++i) {
            m_logView->appendPlainText(QString::fromUtf8(lines.at(i)));
        }
        std::printf("counters: bytes=%llu lines=%llu\n",
                    static_cast<unsigned long long>(m_rxBytes),
                    static_cast<unsigned long long>(m_rxLines));

        // 进度行格式化
        m_logView->appendPlainText(QStringLiteral("已接收 %1 行 / %2 KB")
                                       .arg(m_rxLines)
                                       .arg(m_rxBytes / 1024));

        // ---- 校验点 3：TODO(M2-5) 的时长取值与参数组装 ----
        const QString dur = m_durationCombo->currentData().toString();
        std::printf("duration userData = [%s]  (显示文本 [%s])\n",
                    dur.toUtf8().constData(),
                    m_durationCombo->currentText().toUtf8().constData());

        QStringList args;
        args << QStringLiteral("run")
             << QStringLiteral("--quiet")
             << QStringLiteral("-o") << QStringLiteral("json=-")
             << QStringLiteral("--summary-export")
             << QStringLiteral(BENCHBOARD_ROOT "/reports/summary.json")
             << QStringLiteral("--summary-trend-stats")
             << QStringLiteral("avg,min,med,max,p(90),p(95),p(99),p(99.9)")
             << QStringLiteral("--vus") << QString::number(3)
             << QStringLiteral("--duration") << dur
             << QStringLiteral(BENCHBOARD_ROOT "/examples/script_demo.js");

        m_engine->setProgram(QStringLiteral("C:/Program Files/k6/k6.exe"));
        m_engine->setWorkingDirectory(QStringLiteral(BENCHBOARD_ROOT));
        m_engine->setArguments(args);
        m_engine->setProcessChannelMode(QProcess::SeparateChannels);
        std::printf("args count = %d\n", int(m_engine->arguments().size()));

        // ---- 校验点 4：TODO(M2-4)/(M2-9) ----
        m_engine->kill();   // 对未启动的进程 kill() 是否安全
        std::printf("kill() on idle process: no crash\n");
    }

private slots:
    void onEngineStdout() { m_engine->readAllStandardOutput(); }
    void onEngineStderr() { m_engine->readAllStandardError(); }
    void onEngineFinished(int exitCode, QProcess::ExitStatus exitStatus)
    {
        const bool crashed = (exitStatus == QProcess::CrashExit);
        std::printf("finished: code=%d crashed=%d\n", exitCode, int(crashed));
    }

private:
    QProcess       *m_engine        = nullptr;
    QComboBox      *m_durationCombo = nullptr;
    QPlainTextEdit *m_logView       = nullptr;
    quint64         m_rxBytes       = 0;
    quint64         m_rxLines       = 0;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Probe p;
    std::printf("=== all M2 TODO snippets compile & run ===\n");
    return 0;
}

#include "probe.moc"
