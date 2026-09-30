# 3.22 是测试用的 ENVIRONMENT_MODIFICATION 属性的下限（见 tests/CMakeLists.txt）。
cmake_minimum_required(VERSION 3.22)
project(BenchBoard VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# ---------------------------------------------------------------------------
# 警告配置
#
# 抽成接口目标而不是裸 add_compile_options()：裸写会【泄漏到子目录】——
# add_subdirectory(tests) 之后每个测试目标都会莫名继承 /W4，而你没法只给
# 某一个目标单独加。挂接口目标则粒度明确、可叠加、可单独关掉。
# ---------------------------------------------------------------------------
add_library(benchboard_warnings INTERFACE)

if(MSVC)
    # MSVC 必须显式指定 UTF-8。不写的话源码里的中文（注释、界面字符串）
    # 会被按系统 GBK 代码页解码而乱码。
    target_compile_options(benchboard_warnings INTERFACE /utf-8 /W4)
else()
    target_compile_options(benchboard_warnings INTERFACE -Wall -Wextra)
endif()

option(BENCHBOARD_WERROR "把警告当作错误（CI 用）" OFF)
if(BENCHBOARD_WERROR)
    if(MSVC)
        target_compile_options(benchboard_warnings INTERFACE /WX)
    else()
        target_compile_options(benchboard_warnings INTERFACE -Werror)
    endif()
endif()

# ---------------------------------------------------------------------------
# Qt 定位（三级回退，优先级从高到低）
#
#   ① -DQT_DIR=... / -DCMAKE_PREFIX_PATH=...  显式指定（预置与 CI 走这条）
#   ② 环境变量 QT_DIR（tools/build.sh 用）/ QTDIR（Qt Creator 用）
#   ③ 交给 CMake 自己在 PATH 与常见安装位置里找
#
# 以前是裸 find_package() —— 等于把"Qt 在哪"只留在命令行里，
# 换个入口（预置 / IDE / CI）就得重新解释一遍。
# ---------------------------------------------------------------------------
if(NOT CMAKE_PREFIX_PATH)
    if(DEFINED QT_DIR AND NOT QT_DIR STREQUAL "")
        set(CMAKE_PREFIX_PATH "${QT_DIR}")
    elseif(DEFINED ENV{QT_DIR} AND NOT "$ENV{QT_DIR}" STREQUAL "")
        set(CMAKE_PREFIX_PATH "$ENV{QT_DIR}")
    elseif(DEFINED ENV{QTDIR} AND NOT "$ENV{QTDIR}" STREQUAL "")
        set(CMAKE_PREFIX_PATH "$ENV{QTDIR}")
    endif()
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Charts)

# ---------------------------------------------------------------------------
# 产品代码
#
# 除 main.cpp 外全部收进 benchboard_core：
#   · exe 与 tests/ 下的测试链的是同一个库，不会出现"测的是另一份编译产物"
#   · 新增一个 .cpp 只需要改这一处，不必去每个测试目标里补一行
# ---------------------------------------------------------------------------
add_library(benchboard_core STATIC
    src/K6Engine.cpp
    src/K6Engine.h
    src/LoadEngine.h
    src/MainWindow.cpp
    src/MainWindow.h
    src/MetricsAggregator.cpp
    src/MetricsAggregator.h
    src/RealtimeChartView.cpp
    src/RealtimeChartView.h
    src/ReportWriter.cpp
    src/ReportWriter.h
    src/SummaryParser.cpp
    src/SummaryParser.h
    src/TestOrchestrator.cpp
    src/TestOrchestrator.h
    src/types.h
)

# PUBLIC：链 benchboard_core 的目标自动获得 src/ 的 include 路径
target_include_directories(benchboard_core PUBLIC src)
target_link_libraries(benchboard_core
    PUBLIC  Qt6::Widgets Qt6::Charts
    PRIVATE benchboard_warnings
)

add_executable(BenchBoard WIN32 src/main.cpp)
target_link_libraries(BenchBoard PRIVATE benchboard_core benchboard_warnings)

# 让 exe 输出到 <build>/bin/，tools/build.sh 依赖这个路径
if(WIN32)
    set_target_properties(BenchBoard PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )
endif()

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
