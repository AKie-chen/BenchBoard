# BenchBoard · 可视化压测工作台

> Qt6 桌面应用 · 集成 k6 · 实时曲线 + Markdown 报告

<!-- 📷 截图位（待补）
     放好后取消下面的注释。建议：
       docs/images/main.png   —— 主界面（配置区 + 三张实时曲线 + 汇总表 + 日志）
       docs/images/demo.gif   —— 一次完整压测的录屏（开始 → 曲线滚动 → 导出报告）
     注意 GIF 别太大，10 MB 以内比较合适。

![BenchBoard 主界面](docs/images/main.png)
-->

在界面里填目标 URL、并发数与时长，点「开始压测」，即可看到 RPS / 延迟 / VU 的
秒级实时曲线；压测结束后汇总 p50/p90/p95/p99 与成功率，并一键导出 Markdown
报告。**全程不需要接触命令行。**

---

## 工程上真正难的四件事

界面本身不难。把这个工具做对，难的是下面这四件事 —— 每一条都有实测数据和
可复现的证据，不是"据说"。

### ① 让一个压测进程停下来

第一版用 `QProcess::terminate()` 请求停止，结果**完全无效**：Windows 上
`terminate()` 靠给顶层窗口发 `WM_CLOSE`，而 k6 是无窗口的控制台程序，收不到 ——
3 秒后进程仍在跑，测试还在继续打目标服务。

改用 `kill()` 之后出现第二个问题：**强杀之后 `--summary-export` 留下的
`summary.json` 是【陈旧】的，不是不存在**。按"文件在不在"判断数据有效性，
会静默地把上一轮的结果当成这一轮的报告。所以判据必须是**退出状态**
（`QProcess::ExitStatus`），而不是文件存在性。

> 实测：被强杀时退出码是 `62097`（无语义数字）；`NormalExit` 下才有语义
> （0 = 全通过、99 = thresholds 被突破）。退出码顶不了这个位置。

### ② 解析 k6 的数据流

k6 的 `-o json=-` 会在 stdout 上吐 JSON 行流。三个反直觉的地方：

- **管道边界 ≠ 行边界。** `readyRead` 给到的 chunk 大概率断在某一行中间
  （实测平均每次回调 18~21 行，一条半行就能毁掉一条数据，且频率低到极难复现）。
- **同一条流里有三种累加语义。** `http_reqs` 是 counter，但在**原始流**里发的是
  增量（每个请求一个 `value=1` 的点）；同一个指标在 `summary.json` 里却是累计值。
  当累计值处理，报告里的总请求数会永远是 1。
- **流的末尾混着非 JSON 文本。** k6 跑完会输出 23~30 行人类可读摘要，
  必然解析失败 —— 解析器必须对脏行宽容，而不是遇到就报错。

### ③ 在数据洪流下保持界面响应

实测 k6 最坏能吐 **42,867 行/秒**，`readyRead` 每秒回调约 2,400 次。
每次回调都去 `setText` 的话界面直接假死。

做法是把**数据频率与刷新频率彻底解耦**：回调里只做累加，界面每秒一拍自己去取
增量窗口（拉取，不是推送）。曲线用 `QLineSeries::replace()` 整体替换，
一秒只重绘一次。

> 另一个实测结论：**构建类型是数量级差异。**
> `MetricsAggregator::feed()` 在 Release 下 **330,135 行/秒**，
> Debug 下只有 **14,778 行/秒**（差 23.5×）。所以本项目默认 `RelWithDebInfo`，
> Debug 只用于单步调试；用 Debug 跑压测，10 秒的测试要多花 80 多秒排空积压。

### ④ 让"我实测过"变成"你跑一下就知道"

一个压测工具最容易变成"我说它对它就对"。所以这个仓库里：

- **74 条断言**，`ctest` 一条命令跑完，没装 k6 也能全绿；
- 语料回放按 **1/3/7/64/4096 字节**五种粒度切碎喂进去，结果必须与整块喂**逐字段一致**；
- 有一条**架构守卫**：剥掉注释后断言全仓只有 `K6Engine.h` 认识 `QProcess` ——
  以后谁把进程代码漏进界面层，CI 当场拦下；
- 每条结论都能追到原始输出：`docs/02-实测证据与陷阱速查.md` 是手册，
  `docs/evidence/` 是当时跑出来的截图与日志。

> 这套测试确实抓到了真问题：报告目录曾经因为一个静态初始化顺序问题，
> 被写到 `<当前盘符>:/reports` 而不是可执行文件同级 —— 见 `git log` 里的
> `fix: 报告被写到盘符根目录`。

---

## 功能

- **配置**：目标 URL、并发 VU（1–1000）、时长（10 秒 / 30 秒 / 1 分钟）
- **执行**：一键开始 / 停止，引擎异常即时提示
- **实时曲线**：RPS、延迟（avg + p95）、VU 数，1 秒窗口滚动刷新；
  数据处理与界面刷新解耦，压测期间界面保持响应
- **汇总**：分位数 p90/p95/p99、错误率、总请求数、数据来源标注
- **报告**：导出 Markdown（压测配置 + 结果指标 + 脚本内的 check 结果）

## 环境要求

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Qt | 6.10.2（Widgets + Charts） | 曲线绘制依赖 Charts 模块 |
| MSVC | Visual Studio 2022（x64） | 本机仅在此组合下验证 |
| CMake | ≥ 3.22 | 测试用到 testPresets 与 ENVIRONMENT_MODIFICATION |
| CTest | 随 CMake | `bash tools/build.sh test` 会自动调用 |
| Ninja | 较新版本 | |
| k6 | 建议 v2.x | **外部依赖，需自行安装** |

k6 装在以下任一路径即可被自动探测：

```
C:\Program Files\k6\k6.exe
C:\Program Files (x86)\k6\k6.exe
C:\ProgramData\chocolatey\bin\k6.exe
```

未安装（或装在别处）时，点「开始压测」会被启动前检查拦下并说明原因。

## 构建

```bash
bash tools/build.sh          # 配置 + 构建
bash tools/build.sh run      # 配置 + 构建 + 运行
bash tools/build.sh test     # 配置 + 构建 + 跑测试
bash tools/build.sh clean    # 清理构建目录后重建
```

脚本默认按本机布局探测 Qt 与 Visual Studio；路径不同时用环境变量覆盖：

```bash
QT_DIR="D:/Qt/6.10.2/msvc2022_64" VS_ROOT="/c/VS2022" bash tools/build.sh
```

| 环境变量 | 默认值 | 用途 |
| --- | --- | --- |
| `QT_DIR` | `E:/Qt/6.10.2/msvc2022_64` | Qt 安装目录（Windows 形式） |
| `QT_TOOLS_DIR` | `/e/Qt/Tools` | Qt 自带的 CMake / Ninja 所在目录 |
| `VS_ROOT` | `/d/VS2022` | Visual Studio 安装目录（MSYS 形式） |

也可以直接用 CMake 预置，不经过 `build.sh`：

```bash
export QT_DIR="E:/Qt/6.10.2/msvc2022_64"     # 没设则回落到 QTDIR
cmake --preset default
cmake --build --preset default
ctest --preset default
```

| 预置 | 用途 |
| --- | --- |
| `default` | RelWithDebInfo，日常开发与跑测试 |
| `debug` | Debug，只在需要单步进 Qt/STL 内部时用 |
| `ci` | RelWithDebInfo + `BENCHBOARD_WERROR=ON`，警告即错误 |

预置只负责「生成器 + 构建目录 + 构建类型」，**不负责拼装 MSVC 环境** ——
所以要在已经能编译 C++ 的 shell 里跑（Visual Studio 开发者命令提示符、
Qt Creator 的 kit 环境，或 `vcvars64.bat` 之后的终端）。
本机 `vcvars` 加载不了，`build.sh` 就是为绕开它而存在的（见文件头注释）。

预置的构建目录是 `out/presets/<预置名>`，与 `build.sh` 的 `out/build`、
Qt Creator 的 `out/build/debug`、`out/build/release` 互不覆盖，可以并存。
也可以直接用 Visual Studio 打开根目录的 `CMakeLists.txt`（已附 `CMakeSettings.json`）。

> **不要日常用 `Debug` 跑压测** —— 原因见上面「工程上真正难的四件事」第 ③ 条。

## 使用

1. 起一个被测服务（本地最快的办法）：

   ```bash
   python -m http.server 8899
   ```

2. 打开 BenchBoard，填「目标 URL」（如 `http://127.0.0.1:8899/`），设置并发与时长
3. 点「开始压测」：日志区实时滚动引擎输出，图表区绘制实时曲线
4. 结束后点「导出报告」，Markdown 写到**可执行文件同级的 `reports/` 目录**
   （即 `out/build/bin/reports/`）

压测脚本固定使用 `examples/script_demo.js`：URL、并发、时长由界面经命令行参数
传入，脚本里的默认值只作兜底。

## 项目结构

```
src/                应用源码（界面 / 实时曲线 / 指标聚合 / 报告生成 / k6 引擎适配）
tests/              自动化测试（四层：unit / architecture / ui / integration）
docs/               实测证据与陷阱速查 + 原始证据（截图、日志、探针输出）
examples/           示例 k6 压测脚本
tools/build.sh      命令行构建脚本
tools/probes/       一次性验证探针（"为什么这么写"的实测工具，不参与构建、不进 CI）
```

## 验证

```bash
bash tools/build.sh test     # 或 ctest --preset default
```

| 想知道什么 | 看哪里 |
| --- | --- |
| 测试怎么分层、新功能该在哪层加测试 | [tests/README.md](tests/README.md) |
| 某个设计**为什么**是这样（实测数据、踩过的坑） | [docs/02-实测证据与陷阱速查.md](docs/02-实测证据与陷阱速查.md) |
| 那些结论的原始输出（截图、日志、探针打印） | [docs/evidence/](docs/evidence/) |
| 探针怎么跑、哪个已被测试取代 | [tools/probes/README.md](tools/probes/README.md) |

## 已知限制

- **仅在本机 Windows + MSVC 2022 下验证过。** 仓库里附了 Windows + Ubuntu 的
  CI 配置（`.github/workflows/ci.yml`），但尚未在 GitHub 上实跑 ——
  Linux 侧是这套代码第一次被 GCC 编译，`-Wall -Wextra -Werror` 可能会翻出
  MSVC `/W4` 不报的警告。
- 压测脚本固定为 `examples/script_demo.js`，界面选择自定义脚本的功能尚未实现。
- 引擎只适配 k6（`LoadEngine` 接口已为多引擎预留，但抽象目前是"字节管道"级别——
  换引擎时聚合器仍需改动，见 `src/LoadEngine.h` 的说明）。
- 无多轮次对比视图。
- `TestOrchestrator` 里有两个成员目前只写不读（见头文件注释），
  报告暂时无法标注"本次数据来自实时兜底"。

## 许可证

本项目以 **GPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

- 依赖的 Qt Charts 在 Qt 开源版中采用 **GPLv3**（非 LGPLv3），故本项目整体采用
  GPLv3 兼容许可。
- k6 以**外部进程**方式调用（用户自行安装，AGPLv3），不构成派生作品。
