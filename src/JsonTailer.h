#include <QTimer>
#include <QFile>
#include <QByteArray>

class JsonTailer : public QObject
{
    Q_OBJECT
public:
    explicit JsonTailer(QObject* parent = nullptr);

    bool start(const QString& path);   // 打开文件并启动轮询
    void stop();                       // 停止并关闭
    void reset();                      // 回到文件开头（新一次测试）

signals:
    void lineReady(const QByteArray& line);   // 一条完整的行（不含 '\n'）
    void errorOccurred(const QString& msg);

private slots:
    void onTick();

private:
    QFile       m_file;
    QTimer* m_timer = nullptr;
    qint64      m_offset = 0;      // 已读到的位置
    QByteArray  m_partial;          // 未凑齐的半行
};