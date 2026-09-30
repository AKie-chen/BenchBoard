#include <QMainWindow>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QStatusBar>
#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
int probe();
int probe() { return int(sizeof(QMainWindow) + sizeof(QPushButton) + sizeof(QProcess)); }
