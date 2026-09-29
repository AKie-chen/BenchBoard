#pragma once

// ============================================================================
// 主窗口 —— 界面装配与信号接线
//
// 现在的结构：buildUi() 摆控件 / connectSignals() 接线 /
//   onStartClicked · onStopClicked · onExportClicked 三个动作槽 /
//   onUiTick() 每秒心跳（刷状态栏 + 喂曲线一个增量窗口）/
//   以及编排层四个信号的接收槽（stateChanged · logMessage · runFinished · failed）。
//
// ★ 这个文件里【搜不到 QProcess】—— 进程、参数拼装、字节流全在 K6Engine 里，
//   界面只通过 TestOrchestrator 的公开方法与信号交互。判据不是 grep（注释里
//   出现 QProcess 也会被数进去），而是"删掉 #include <QProcess> 后仍能编译"。
//
// ★ 数据链路是"累加 → 取增量 → 画点"：
//   MetricsAggregator 累加（在编排层内部），takeWindow() 取本秒增量，onUiTick() 喂给曲线。
//
// 分层演进（每一步都是"痛了才拆"，不是"提前设计好"）：
//   M4 加「RealtimeChartView 成员」   —— 屏幕上出现第一条曲线
//   M5 加「汇总表格 + 终态数据槽」    —— 出 p95 / p99
//   M7 把 QProcess 挪走，换成 TestOrchestrator 成员（这一步叫「重构」）
//
// 规则：任何时刻这个文件都必须是「能编译通过」的状态。
// 不要提前把还没实现的类写进来，那只会换来 LNK2019。
// ============================================================================

#include "types.h"

#include <QMainWindow>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;          // 只需要指针成员，前置声明就够
class QTableWidget;
QT_END_NAMESPACE

class RealtimeChartView;   // 同上，指针成员只需要前置声明
class TestOrchestrator;    // 编排层：界面唯一的后端入口

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private slots:
    void onStartClicked();   // 读参数 → 交给编排层起一轮压测
    void onStopClicked();    // 请编排层停（底层是 kill()，terminate() 对 k6 无效，实测）
    void onExportClicked();   // 导出 Markdown 报告

    // 由 QTimer 每秒触发一次：刷状态栏 + 喂曲线一个增量窗口。
    // ★ 界面刷新【只在这里发生】，绝不在数据回调里 setText ——
    //   那是"回调频率 ≠ 显示频率"这条纪律的落点。
    void onUiTick();

    // --- 编排层的四个信号 ---
    void onStateChanged(bool running);
    void onLogMessage(const QString &line);
    void onRunFinished(const TestRunResult &result, const QString &summaryError);
    void onFailed(const QString &reason);

private:
    void buildUi();                        // 把控件摆到窗口上
    void connectSignals();                 // 信号槽接线
    void appendLog(const QString &line);   // 统一的日志追加入口

    void setRunning(bool running);         // 统一管按钮的可用性、状态栏文字

    // 汇总面板：数据直接来自编排层交下来的结果（唯一取值入口在编排层）。
    // summaryError 非空表示终值解析失败、走了实时兜底。
    void fillSummaryTable(const TestRunResult &result, const QString &summaryError);

    // --- 配置区控件，全部在 buildUi() 里 new 出来 ---
    QLineEdit      *m_urlEdit       = nullptr;   // 被测 URL（见 K6Engine::buildArguments 的 -e BASE_URL=）
    QSpinBox       *m_vusSpin       = nullptr;   // 并发 VU 数
    QComboBox      *m_durationCombo = nullptr;   // 压测时长
    QPushButton    *m_startButton   = nullptr;   // 「开始压测」按钮
    QPlainTextEdit *m_logView       = nullptr;   // 日志 / 引擎输出
    QLabel         *m_statusLabel   = nullptr;   // 状态栏右侧：引擎状态

    QPushButton *m_stopButton   = nullptr;   // 「停止」按钮
    QPushButton *m_exportButton = nullptr;   // 「导出报告」按钮

    // ★ 界面唯一的后端入口，也是界面持有的唯一"业务"对象。
    //   它必须建在 buildUi() / connectSignals() 【之前】—— connectSignals() 里
    //   要 connect 它的信号，那一刻它不能是 nullptr（否则 Qt 只打一行警告就跳过）。
    TestOrchestrator *m_orchestrator = nullptr;

    QTimer *m_uiTimer = nullptr;   // 每秒一跳

    // 曲线区。这里用【指针】而不是值成员，因为 RealtimeChartView 继承 QWidget，
    // 它需要 parent 才能被 Qt 的父子机制接管生命周期 —— 值成员拿不到 this。
    RealtimeChartView *m_chartView = nullptr;

    QTableWidget *m_summaryTable = nullptr;   // 汇总表格
};
