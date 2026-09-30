# 测试

```
bash tools/build.sh test          # 配置 + 构建 + 跑全部测试
ctest --preset default            # 或者走 CMake 预置
```

没装 k6 也能全绿 —— 需要 k6 的检查会 `QSKIP`，不算失败。

## 四层

用 CTest 标签区分，目的是让 CI 和本地都能只跑"该跑的那部分"：

| 标签 | 目录 | 跑什么 | 依赖 |
| --- | --- | --- | --- |
| `unit` | `unit/` | 纯逻辑：聚合器、解析器、报告生成、参数拼装 | 只要 Qt Core（`tst_orchestrator` 除外） |
| `architecture` | `architecture/` | 读源码文本做断言（分层守卫） | 不跑代码，只读文件 |
| `ui` | `ui/` | 界面 → 编排层 → 报告的端到端接线 | Qt Widgets + Charts，`QT_QPA_PLATFORM=offscreen` |
| `integration` | `integration/` | 真跑 k6 | 本机装 k6（暂未建立） |

```bash
ctest --test-dir out/build -L unit            # 只跑一层
ctest --test-dir out/build -LE integration    # 跳过某层
ctest --test-dir out/build -R tst_layering    # 只跑一个
bash tools/build.sh test -L unit              # 也可以直接透传给 ctest
```

单个测试的详细输出用 `-o 文件,txt` 看（Git Bash 下 Qt 控制台程序的 stdout 有时附着不上终端）：

```bash
./out/build/tests/tst_layering.exe -o /tmp/t.txt,txt && cat /tmp/t.txt
```

## 加测试该放哪一层

按**被测对象**判断，不按"跑得快不快"判断：

- 测一个**纯函数 / 数据类**（不碰进程、不碰界面） → `unit/`
- 测一条**结构约束**（"谁能 include 谁""谁认识 QProcess"） → `architecture/`
- 测**界面与后端的接线**（信号槽、控件状态、导出产物） → `ui/`
- 需要**真的拉起 k6** 才能验的 → `integration/`（目前还没有）

## 现有测试的职责

| 文件 | 守什么 |
| --- | --- |
| `smoke/tst_smoke.cpp` | 测试基础设施本身（路径宏、库链接、CTest 注册） |
| `unit/tst_metricsaggregator.cpp` | 切行粒度不影响结果；半行残留；三种累加语义；reset 清水位 |
| `unit/tst_summaryparser.cpp` | summary.json 的三个层级/类型陷阱；缺字段 ≠ 解析失败 |
| `unit/tst_reportwriter.cpp` | 文件名安全；正文内容契约；落盘（无 BOM / LF / 16KB 不截断 / 失败要报错） |
| `unit/tst_engine.cpp` | `buildArguments` 的每一条参数；引擎可判定状态 |
| `architecture/tst_layering.cpp` | **全仓只有 `K6Engine.h` 认识 `QProcess`** |
| `ui/tst_orchestrator.cpp` | 报告里的值 == 本次输入（不可伪造）；强杀后不沿用陈旧的 summary.json |

## 写测试的规矩

**① 不许出现绝对路径。** 一切路径从 `support/TestPaths.h` 取：

```cpp
#include "TestPaths.h"
TestPaths::sourceDir()          // 仓库根
TestPaths::corpus("xxx.json")   // tests/corpus/xxx.json
TestPaths::src("MainWindow.cpp")// src/MainWindow.cpp
```

两个根目录由 CMake 的 `benchboard_test_support` 以编译期定义传入，值来自
`${CMAKE_SOURCE_DIR}` —— 换机器、换 clone 路径都不用改代码。
（旧探针把 `E:/BenchBoard/...` 硬编码了 103 处、个人临时目录 42 处，
换台机器就没有一条断言跑得起来 —— 这是这条规矩的由来。）

**② 不许出现裸数字。** 语料的对账期望值集中在 `support/Corpus.h`：

```cpp
#include "Corpus.h"
const Corpus::Expectation want;
QCOMPARE(qint64(got.points), want.points);   // 而不是写死 1986
```

**③ 从报告里取值一律走 `support/MarkdownFields.h`：**

```cpp
#include "MarkdownFields.h"
MarkdownFields::value(md, QStringLiteral("总请求数"))
MarkdownFields::has(md, QStringLiteral("目标 URL"))   // 区分"行不存在"和"值为空"
```

它做两件容易被忽略的事：剥掉 Markdown 的 `-` / `#` 标记，以及**整字段名精确匹配**
（前缀匹配会让"错误"命中"错误率"—— 踩过）。

**④ 断言挑"不可伪造"的判据。** 优先用**有默认值**的字段做等值比较：
`TestConfig` 的 `vus` 默认 10、`QSpinBox` 初值 1，测试里填 7 ——
那么"报告里是 7"只可能来自真实接线。"非空"会被默认值骗过。

**⑤ 需要外部工具的检查要 `QSKIP` 而不是失败：**

```cpp
if (K6Engine::detectK6Path().isEmpty())
    QSKIP("本机未安装 k6：跳过引擎探测一致性检查（不是失败）");
```

**⑥ 类名不要和产品类重名。** 测试类叫 `TestOrchestrator` 会撞上产品类
`TestOrchestrator`，moc 生成的元对象会张冠李戴，报出一屏和真实问题
毫不相干的"使用未定义类型"错误（实测踩过）。本项目约定：`tst_orchestrator.cpp`
里的类叫 `TestOrchestratorWiring`。

## 加语料

1. 把 k6 的真实产出丢进 `tests/corpus/`
2. 在 `support/Corpus.h` 加一个 `inline QString xxxFile()` 取路径函数
3. 期望值在测试里用 `_data()` 数据表逐行列出来，别散在断言中间

语料是**只读**的：测试不许改写 `tests/corpus/` 下的任何文件。

## 已知的脆弱点

- **`ui/tst_orchestrator.cpp` 是契约测试，不是单元测试。** 它靠两个技巧驱动界面：
  把 URL 置空让 `preflight()` 早退（避免真起 k6 进程产生孤儿），
  再用 `QMetaObject::invokeMethod` 直调编排层的私有槽 `onEngineFinished`。
  **内部结构一变它就会红** —— 这是刻意的取舍，换来的是那批"不可伪造"的接线判据。
  改 `MainWindow` / `TestOrchestrator` 的内部结构时，预期需要同步更新它。
- **它往 `<测试 exe 同级>/reports/` 写产物。** 这落在构建目录里（`out/`，已被 gitignore），
  且 `initTestCase` / `cleanupTestCase` 会记账并复原：只删本次自己创建的 `.md`，
  `summary.json` 会先挪开再放回。跑单个用例时产物不会残留。
