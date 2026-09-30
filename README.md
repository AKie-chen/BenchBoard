# BenchBoard · 可视化压测工作台

> Qt6 桌面应用 · 集成 k6 · 实时曲线 + Markdown 报告

在界面里填目标 URL、并发数与时长，点「开始压测」，即可看到 RPS / 延迟 / VU 的
秒级实时曲线；压测结束后汇总 p50/p90/p95/p99 与成功率，并一键导出 Markdown
报告。**全程不需要接触命令行。**

## 功能

- **配置**：目标 URL、并发 VU（1–1000）、时长（10 秒 / 30 秒 / 1 分钟）
- **执行**：一键开始 / 停止，引擎异常即时提示
- **实时曲线**：RPS、平均延迟、VU 数，1 秒窗口滚动刷新；数据处理与界面刷新解耦，
  压测期间界面保持响应
- **汇总**：分位数 p50/p90/p95/p99、成功率、总请求数、吞吐量
- **报告**：导出 Markdown（压测配置 + 结果指标 + 脚本内的阈值检查项）

## 环境要求

| 依赖 | 版本 | 说明 |
| --- | --- | --- |
| Qt | 6.10.2（Widgets + Charts） | 曲线绘制依赖 Charts 模块 |
| MSVC | Visual Studio 2022（x64） | 目前仅在 Windows + MSVC 下验证 |
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

未安装（或装在别处）时，点「开始压测」会被启动前检查拦下并提示原因。

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

注意预置只负责「生成器 + 构建目录 + 构建类型」，**不负责拼装 MSVC 环境**——
所以要在已经能编译 C++ 的 shell 里跑（Visual Studio 开发者命令提示符、
Qt Creator 的 kit 环境，或 `vcvars64.bat` 之后的终端）。
本机 `vcvars` 加载不了，`build.sh` 就是为绕开它而存在的（见文件头注释）。

| 预置 | 用途 |
| --- | --- |
| `default` | RelWithDebInfo，日常开发与跑测试 |
| `debug` | Debug，只在需要单步进 Qt/STL 内部时用 |
| `ci` | RelWithDebInfo + `BENCHBOARD_WERROR=ON`，警告即错误 |

预置的构建目录是 `out/presets/<预置名>`，与 `build.sh` 的 `out/build`、
Qt Creator 的 `out/build/debug`、`out/build/release` 互不覆盖，可以并存。

也可以直接用 Visual Studio 打开根目录的 `CMakeLists.txt`（已附
`CMakeSettings.json`）。

> 构建类型默认 `RelWithDebInfo`，**不建议日常用 `Debug`**：Debug 版 Qt DLL
> 叠加 `/Od` 与调试 CRT 后，数据聚合吞吐只有约 1.5–2 万行/秒，RelWithDebInfo
> 则可达 33 万行/秒。同样的压测，Debug 下差距会直接表现为界面卡顿。

## 使用

1. 起一个被测服务（本地最快的办法）：

   ```bash
   python -m http.server 8899
   ```

2. 打开 BenchBoard，填「目标 URL」（如 `http://127.0.0.1:8899/`），设置并发与时长
3. 点「开始压测」：日志区实时滚动引擎输出，图表区绘制 RPS 曲线
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

## 当前限制

- 仅在 Windows + MSVC 2022 下验证过；未在 macOS / Linux 构建
- 压测脚本目前固定为 `examples/script_demo.js`，界面选择自定义脚本的功能在开发中
- 引擎只适配 k6（`LoadEngine` 接口已为多引擎预留）
- 无多轮次对比视图

## 许可证

本项目以 **GPL-3.0** 发布，全文见 [LICENSE](LICENSE)。

- 依赖的 Qt Charts 在 Qt 开源版中采用 **GPLv3**（非 LGPLv3），故本项目整体采用
  GPLv3 兼容许可。
- k6 以**外部进程**方式调用（用户自行安装，AGPLv3），不构成派生作品。
