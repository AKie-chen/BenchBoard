#include <QtCore>
#include <QtGui>
#include <QtWidgets>
int probe();
int probe() { return int(sizeof(QMainWindow) + sizeof(QPushButton) + sizeof(QProcess)); }
