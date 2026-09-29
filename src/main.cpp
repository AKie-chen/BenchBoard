#include "MainWindow.h"

#include <QApplication>

// 唯一职责：起事件循环 + 显示主窗口。窗口内容全部在 MainWindow 里搭。
int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BenchBoard"));
    QApplication::setApplicationVersion(QStringLiteral("0.1"));

    MainWindow window;
    window.resize(1240, 840);
    window.setWindowTitle(QStringLiteral("BenchBoard - 可视化压测工作台"));
    window.show();

    return app.exec();
}
