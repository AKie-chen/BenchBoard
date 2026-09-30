// 反例：只前置声明 QProcess，然后想用它的嵌套类型 QProcess::ExitStatus
// 实测报错：error C2027: 使用了未定义类型"QProcess"
//           error C2061: 语法错误: 标识符"ExitStatus"
// 结论：槽签名里出现 QProcess::ExitStatus 时，MainWindow.h 必须 #include <QProcess>
class QProcess;
void onFinished(int exitCode, QProcess::ExitStatus st);
int probeE();
int probeE() { return 0; }
