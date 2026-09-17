#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();
};
#endif // MAINWINDOW_H
