# 探针（probes）

**这里放的是开发过程中用来"问清楚一件事"的一次性程序，不是回归测试。**

区别说清楚：

| | `tests/` | `tools/probes/` |
| --- | --- | --- |
| 性质 | 自动化回归 | 手工复现工具 |
| 什么时候跑 | 每次改动 | 想复现某个结论时 |
| 判据 | 退出码（CTest 认） | 看它打印的表格 / 生成的 PNG |
| 长期维护 | 是 —— 破坏它会红 | 否 —— 允许随着 `src/` 演进而失效 |

它们存在的原因是：本项目里很多结论**不能靠推理得出，只能实测**——
"`terminate()` 对 k6 到底有没有用""栈上放 QChart 会不会崩""主线程扛不扛得住 33 万行/秒"。
探针就是当时回答这些问题留下的原始工具，结论整理在
[`docs/02-实测证据与陷阱速查.md`](../../docs/02-实测证据与陷阱速查.md)，原始输出在
[`docs/evidence/`](../../docs/evidence/)。

## 已经被 `tests/` 取代的

这几个探针的断言已经 1:1 搬进自动化测试，**回归请跑测试，不要跑探针**：

| 探针 | 现在由谁守 |
| --- | --- |
| `corpus_replay_probe.cpp` | `tests/unit/tst_metricsaggregator.cpp` |
| `summary_parser_probe.cpp` | `tests/unit/tst_summaryparser.cpp` |
| `m6_report_probe.cpp` | `tests/unit/tst_reportwriter.cpp` + `tests/ui/tst_orchestrator.cpp` |
| `m7_engine_probe.cpp` | `tests/unit/tst_engine.cpp` + `tests/architecture/tst_layering.cpp` |
| `ui_assert_probe.cpp` | 部分并入 `tests/ui/tst_orchestrator.cpp` |

```bash
ctest --test-dir out/build -R tst_metricsaggregator --output-on-failure
```

## 仍然只有探针能做的

| 探针 | 回答的问题 |
| --- | --- |
| `qprocess_start_check.cpp`<br>`qprocess_startdiag.cpp`<br>`qprocess_channel_diag.cpp` | k6 到底起没起来？起不来的话卡在哪一步？（四路对照 + 通道模式诊断） |
| `firehose_replay_bench.cpp` | `MetricsAggregator::feed()` 在 Release / Debug 下的真实吞吐差多少 |
| `firehose_ui_probe.cpp` | 「k6 早就跑完了，界面还要多久才承认」—— 排空积压耗时的客观度量 |
| `k6_firehose_bench.cpp` | 主线程 + 异步读 + 轻量行扫描能不能扛住 k6 的火管 |
| `k6_stop_behavior.cpp` | 对 k6 调 `terminate()` / `kill()` 分别发生什么 |
| `m5_chart_paint_probe.cpp` | 「有轴、有网格、没有曲线」—— 按**彩色像素数**定位，不靠肉眼 |
| `m5_chartcontext_probe.cpp` | `ChartContext` 的成员为什么只能放指针 |
| `m4_stack_owner_probe.cpp` | Qt Charts 的所有权链上，到底哪个对象栈化会崩 |
| `stack_layout_probe.cpp` | `QVBoxLayout` 用栈对象到底会怎样 |
| `m4_window_probe.cpp` | 窗口拉伸时"只有日志区变高"的守恒断言 |
| `m3_e2e_probe.cpp` / `m2_e2e_probe.cpp` | 真跑 k6 的端到端：状态栏数字在跳，且请求数逐秒单调不减 |
| `m4_skeleton_check.cpp`<br>`qt_api_snippets_check.cpp` | 里程碑期的骨架自测（历史遗留，价值最低） |
| `include_style_bench/` | 头文件风格对编译期的影响（见该目录自己的 README） |

## 配套脚本

| 文件 | 用途 |
| --- | --- |
| `fast_http_server.js` | 压测用的「快目标」—— 目的是把 k6 的 JSON 输出拉到万级 rps |
| `script_normal.js` / `script_hammer.js` | 正常 / 高频两种压测脚本 |
| `k6_threshold_semantics.js` | 探 k6 thresholds 的判定语义 |

## 怎么跑

每个探针配一个 `xxx_run.sh`，它把源码复制到临时目录、拼好 MSVC 环境、构建并运行：

```bash
bash tools/probes/m5_chart_paint_probe_run.sh
```

产物（PNG / 日志）落在 `out/probes-tmp/<探针名>/`。

**路径与工具链全部可以用环境变量覆盖**，默认值是本机布局：

| 环境变量 | 默认值 | 用途 |
| --- | --- | --- |
| `QT_DIR` | `E:/Qt/6.10.2/msvc2022_64` | Qt 安装目录 |
| `QT_TOOLS_DIR` | `/e/Qt/Tools` | Qt 自带的 CMake / Ninja |
| `VS_ROOT` / `VS_ROOT_WIN` | `/d/VS2022` / `D:/VS2022` | Visual Studio 安装目录 |
| `NODE` / `PYTHON` | `node` / `python` | 少数探针要起本地 HTTP 服务 |

仓库根目录由脚本自己从 `BASH_SOURCE` 反推，不用配。

## 注意

- 这些脚本**只为 Git Bash / MSYS 环境写**，且假设 MSVC + Windows SDK 装在默认位置。
  它们不参与 CI，也不在 `ctest` 里 —— 挂了不影响构建。
- 探针**不做版本维护**。`src/` 的接口变了它们就可能编不过；
  真要复现某个历史结论时，用 `git log` 找到当时的 `src/` 再跑。
- 探针的首要约束是**不改变被测系统**：会写文件的那些（`m6_report_probe`、
  `m5_ui_probe`）跑完会把自己创建的产物清掉或用备份还原。
