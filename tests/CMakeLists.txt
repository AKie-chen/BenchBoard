# ============================================================================
# 测试工程
#
# 分四层，用 CTest 标签区分 —— 这样 CI 可以只跑不依赖外部环境的那几层：
#   unit          纯逻辑，毫秒级，任何机器都能跑
#   architecture  架构约束（读源码文本做断言），不起进程
#   ui            Qt Widgets，靠 QT_QPA_PLATFORM=offscreen 无头运行
#   integration   需要本机装了 k6；没装则 QSKIP，不算失败
#
# 跑法：
#   ctest --test-dir out/presets/default --output-on-failure   # 全部
#   ctest --test-dir out/presets/default -L unit               # 只跑一层
#   ctest --test-dir out/presets/default -LE integration       # 跳过集成层
# ============================================================================

find_package(Qt6 REQUIRED COMPONENTS Test)

# ---------------------------------------------------------------------------
# 测试公共设施：路径的唯一来源
#
# 旧探针把 E:/BenchBoard/... 硬编码了 103 处、.workbuddy 路径 42 处 ——
# 换台机器就没有一条断言跑得起来。这里把根目录做成编译期定义，
# 测试代码只通过 TestPaths:: 取路径，【不允许出现绝对路径】。
# ---------------------------------------------------------------------------
add_library(benchboard_test_support INTERFACE)
target_include_directories(benchboard_test_support INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_compile_definitions(benchboard_test_support INTERFACE
    "BENCHBOARD_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
    "BENCHBOARD_CORPUS_DIR=\"${CMAKE_SOURCE_DIR}/tests/corpus\""
)
target_link_libraries(benchboard_test_support INTERFACE benchboard_warnings)

# 注册一个测试目标。
# 用显式的 add_executable + add_test 而不是 qt_add_test：后者对 Qt 版本有
# 要求，前者在任何 Qt6 上行为一致 —— 测试基础设施要的是可预测。
function(benchboard_add_test name)
    cmake_parse_arguments(ARG "" "LABEL" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE
        Qt6::Test benchboard_core benchboard_test_support ${ARG_LIBS})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES LABELS "${ARG_LABEL}")

    # ★ 把 Qt 的 bin 目录挂到测试进程的 PATH 上。
    #
    #   Windows 下测试 exe 与 Qt6Core.dll / Qt6Test.dll 不在同一个目录，
    #   不挂就是 0xc0000135（STATUS_DLL_NOT_FOUND）——进程根本起不来，
    #   CTest 只会报一个裸的十六进制退出码，看不出原因。
    #
    #   为什么用 ENVIRONMENT_MODIFICATION 而不是 ENVIRONMENT：
    #   PATH 本身是分号分隔的列表，塞进 ENVIRONMENT 需要手工转义分号，
    #   而 $ENV{PATH} 里的分号没法自动转义 —— 一改就碎。path_list_prepend
    #   就是为这件事设计的，由 CMake 负责按平台的分隔符拼接。
    #
    #   用 $<TARGET_FILE_DIR:...> 而不是硬编码 Qt 路径：换 Qt 版本 / 换机器
    #   都不用改，也避免把本机路径写进仓库。
    set_tests_properties(${name} PROPERTIES
        ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")

    # UI 层统一走离屏后端：CI 上没有显示器，不设这个会直接起不来
    if(ARG_LABEL STREQUAL "ui")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endfunction()

# --- 冒烟：先证明"测试基础设施本身是通的"，再往里灌真实断言 ---
benchboard_add_test(tst_smoke
    LABEL   unit
    SOURCES smoke/tst_smoke.cpp
)

# --- 纯逻辑层：毫秒级，不碰进程、不碰界面、不依赖外部工具 ---
benchboard_add_test(tst_metricsaggregator
    LABEL   unit
    SOURCES unit/tst_metricsaggregator.cpp
)
benchboard_add_test(tst_summaryparser
    LABEL   unit
    SOURCES unit/tst_summaryparser.cpp
)
benchboard_add_test(tst_reportwriter
    LABEL   unit
    SOURCES unit/tst_reportwriter.cpp
)
benchboard_add_test(tst_engine
    LABEL   unit
    SOURCES unit/tst_engine.cpp
)

# --- 架构约束层：读源码文本做断言，不起任何进程 ---
benchboard_add_test(tst_layering
    LABEL   architecture
    SOURCES architecture/tst_layering.cpp
)

# --- 界面层：离屏运行（QT_QPA_PLATFORM=offscreen 由 benchboard_add_test 设置）---
benchboard_add_test(tst_orchestrator
    LABEL   ui
    SOURCES ui/tst_orchestrator.cpp
)
