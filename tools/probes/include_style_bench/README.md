# include 写法代价对比探针

验证 `docs/02-实测证据与陷阱速查.md` 第 3.8 节的数据。

四个 target 内容几乎相同（都只使用 QMainWindow / QPushButton / QProcess），
唯一区别是 include 写法：

| target | 写法 |
| --- | --- |
| A | `#include <QMainWindow>` 逐类包含 |
| B | `#include <QtWidgets>` 模块总头 |
| C | `#include <QtWidgets/QMainWindow>` 模块/类 形式 |
| D | `#include <QtCore> + <QtGui> + <QtWidgets>` 三个总头 |

跑法（在 Git Bash 里，MSVC+Qt 环境由脚本自己拼）：

```bash
# configure + 全量构建
cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="E:/Qt/6.10.2/msvc2022_64" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
cmake --build out

# 单个 target 重编计时（删 obj 再 build）
rm -f out/CMakeFiles/B.dir/B.cpp.obj && time ninja -C out B
```

注意：CMake 给 MSVC 默认加 `/showIncludes`，所以构建日志里能看到每个
编译单元实际拉进了哪些头文件，可以直接数——这就是"牵扯头文件数"的来源。

`E_nested_type_fails.cpp` 是反例：单独编译它会报 C2027 + C2061，证明前置声明
不足以使用 `QProcess::ExitStatus` 这类嵌套类型。
