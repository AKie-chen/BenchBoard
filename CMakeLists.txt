cmake_minimum_required(VERSION 3.19)
project(BenchBoard VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# MSVC 必须显式指定 UTF-8。
# 不写的话源码里的中文（注释、界面字符串）会被按系统 GBK 代码页解码而乱码。
# 已实测：加上后中文以 UTF-8 字节输出（E7 BB 93 = "结"）。
if (MSVC)
    add_compile_options(/utf-8 /W4)
endif()

# ---------------------------------------------------------------------------
# 【当前阶段：M4 —— 画第一条曲线】
#
# TODO(M4-2) 本关你要改的就是下面这两处（★ 少改一处 = 链接期一堆未定义符号）：
#     find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)
#     target_link_libraries(... Qt6::Widgets Qt6::Charts)
#
# 为什么是"两处"而不是一处：find_package 负责让 CMake【找到】QtCharts 并把
# 它的 include 目录加进来（编译期），target_link_libraries 负责把库连上（链接期）。
# 只改第一处 → 编译过、链接报 QChartView 未定义；
# 只改第二处 → CMake 直接报 "Qt6::Charts not found"。
#
# ★ Qt Charts 在 Qt 开源版是 GPLv3，README 里要声明（M7 收尾时补）。
# ---------------------------------------------------------------------------
find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)

# ---------------------------------------------------------------------------
# 每完成一个里程碑，就往这里加一行；目标是不让任何时刻存在
# 「源文件已存在但实现还没写」→ LNK2019 的状态。
# ---------------------------------------------------------------------------
set(BENCHBOARD_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/MetricsAggregator.cpp   # M3 新增
    src/MetricsAggregator.h
    src/types.h                 # M4 新增：从 _target/ 毕业，只搬了 WindowSample
    src/RealtimeChartView.cpp   # M4 新增：骨架已就位（空实现），你往里填
    src/RealtimeChartView.h
    src/SummaryParser.cpp       # M5 新增：从 _target/ 毕业
    src/SummaryParser.h
    src/ReportWriter.cpp       
    src/ReportWriter.h
    src/K6Engine.cpp             # M7 新增：从 _target/ 毕业（引擎实现）
    src/K6Engine.h
    src/LoadEngine.h
    src/TestOrchestrator.cpp
    src/TestOrchestrator.h
)

add_executable(BenchBoard WIN32 ${BENCHBOARD_SOURCES})

target_include_directories(BenchBoard PRIVATE src)

target_link_libraries(BenchBoard PRIVATE
    Qt6::Widgets
    Qt6::Charts
)

# 让 exe 输出到 out/build/bin/，tools/build.sh 依赖这个路径
if (WIN32)
    set_target_properties(BenchBoard PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()
