#!/usr/bin/env bash
# ============================================================================
# BenchBoard 命令行构建脚本（Git Bash / MSYS 环境）
#
# 为什么需要这个脚本：
#   本机的 MSVC + Qt 组合从命令行直接构建会踩三个坑，本脚本全部绕开：
#     1. vcvars 加载失败（Enter-VsDevShell 报 Path/PATH 大小写重复键）
#        → 改为手工拼 INCLUDE / LIB / PATH，不调用 vcvars
#     2. PATH 太脏会被 MSYS 的 /usr/bin/link.exe 顶掉 MSVC 的 link.exe，
#        报 "ld.exe: cannot find /nologo /out:..."（看着像 mingw 的锅）
#        → PATH 只保留 MSVC + SDK + CMake + Ninja + System32
#     3. -DCMAKE_PREFIX_PATH=/e/Qt/... 这种 MSYS 路径形式不被 CMake 识别
#        → 必须写成 E:/Qt/... 的 Windows 形式
#
# 用法：
#   bash tools/build.sh          # 配置 + 构建
#   bash tools/build.sh run      # 配置 + 构建 + 运行
#   bash tools/build.sh clean    # 删除构建目录后重建
# ============================================================================
set -u

export PATH="/usr/bin:/bin:$PATH"   # Git Bash 缺了这行连 mkdir/dirname 都没有

# ★ 必须用 pwd -W 拿 Windows 形式（E:/BenchBoard）。
#   原生 cmake.exe 不认识 MSYS 的 /e/BenchBoard，会报
#   "The source directory /e/BenchBoard does not exist"。
#   MSYS 工具（rm/ls）两种形式都能处理，所以统一用 Windows 形式没问题。
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && (pwd -W 2>/dev/null || pwd))"
# ★ 工具链位置：默认按本机布局，可用环境变量覆盖（见 README）。
#   Qt 有【两个形式】不能混用：CMake 只认 Windows 形式（E:/...），
#   PATH 只认 MSYS 形式（/e/...）—— 所以同一个 Qt 目录在这里存两份。
QT_DIR="${QT_DIR:-E:/Qt/6.10.2/msvc2022_64}"
QT_DIR_MSYS="$(cygpath -u "$QT_DIR" 2>/dev/null || echo "/e/Qt/6.10.2/msvc2022_64")"
QT_TOOLS_DIR="${QT_TOOLS_DIR:-/e/Qt/Tools}"    # Qt 自带的 CMake / Ninja 所在目录
BUILD_DIR="$ROOT/out/build"

# ---------- 自动探测工具链版本（避免升级 VS 后脚本失效）----------
VS_ROOT="${VS_ROOT:-/d/VS2022}"                                          # MSYS 形式：用于探测版本
VS_ROOT_WIN="$(cygpath -m "$VS_ROOT" 2>/dev/null || echo "D:/VS2022")"   # Windows 形式：给 INCLUDE/LIB
MSVC_VER="$(ls "$VS_ROOT/VC/Tools/MSVC/" 2>/dev/null | sort -V | tail -1)"
SDK_VER="$(ls "/c/Program Files (x86)/Windows Kits/10/Include/" 2>/dev/null | grep '^10\.' | sort -V | tail -1)"

if [ -z "$MSVC_VER" ] || [ -z "$SDK_VER" ]; then
  echo "!! 无法探测 MSVC 或 Windows SDK，请检查 $VS_ROOT 与 Windows Kits 安装" >&2
  exit 1
fi
echo ">> 工具链: MSVC $MSVC_VER / SDK $SDK_VER"

MSVC_WIN="$VS_ROOT_WIN/VC/Tools/MSVC/$MSVC_VER"
SDK_WIN="C:/Program Files (x86)/Windows Kits/10"

# ★ PATH 里必须是 /d/ /c/ 的 MSYS 形态；"D:/..." 写进 PATH 不生效
CLEANPATH="$VS_ROOT/VC/Tools/MSVC/$MSVC_VER/bin/HostX64/x64"
CLEANPATH="$CLEANPATH:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64"
CLEANPATH="$CLEANPATH:$QT_TOOLS_DIR/CMake_64/bin:$QT_TOOLS_DIR/Ninja"
CLEANPATH="$CLEANPATH:/c/Windows/System32:/c/Windows"

export INCLUDE="$MSVC_WIN/include;$SDK_WIN/Include/$SDK_VER/ucrt;$SDK_WIN/Include/$SDK_VER/shared;$SDK_WIN/Include/$SDK_VER/um;$SDK_WIN/Include/$SDK_VER/winrt"
export LIB="$MSVC_WIN/lib/x64;$SDK_WIN/Lib/$SDK_VER/ucrt/x64;$SDK_WIN/Lib/$SDK_VER/um/x64"

if [ "${1:-}" = "clean" ]; then
  echo ">> 清理 $BUILD_DIR"
  rm -rf "$BUILD_DIR"
  shift
fi

# ---------- 配置 ----------
# 注意 CMAKE_PREFIX_PATH 用 E:/ 形式，不能用 /e/ 形式（实测 /e/ 形式会导致
# "Could not find a package configuration file provided by Qt6"）
#
# ★★ 构建类型：默认 RelWithDebInfo，【不要】默认 Debug ★★
#   2026-09-23 实测（docs/02 §3.15）：
#     · Debug 配置下 Qt 的 CMake 包会把 Qt6::Core/Gui/Widgets 映射到
#       【Debug 版 DLL】（Qt6Cored.dll / Qt6Widgetsd.dll），叠加 /Od 与
#       debug CRT 的堆开销，MetricsAggregator::feed() 只有 1.5~2.1 万行/s；
#     · RelWithDebInfo 是 33 万行/s —— 差 16~23 倍。
#   而 k6 在 8VU/10s 下真的会吐 139 万行（≈350MB）→ Debug 下就是
#   「10 秒的测试要 60 多秒才结束，界面全程无响应」。
#
#   所以：开发循环用 RelWithDebInfo（仍带 /Zi + PDB，能下断点看变量），
#   【只有】真的需要在 Qt/STL 内部单步时才临时切：
#       bash tools/build.sh debug          # 或 BUILD_TYPE=Debug bash tools/build.sh
#   切回去之后请重跑一次带性能判据的验收 —— Debug 下的耗时数字没有参考价值。
BUILD_TYPE="${BUILD_TYPE:-RelWithDebInfo}"
if [ "${1:-}" = "debug" ]; then
  BUILD_TYPE="Debug"; shift
fi
echo ">> 构建类型: $BUILD_TYPE"

PATH="$CLEANPATH" cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DCMAKE_PREFIX_PATH="$QT_DIR" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
CFG=$?
[ $CFG -ne 0 ] && echo "!! configure 失败" && exit $CFG

# ---------- 构建 ----------
PATH="$CLEANPATH" cmake --build "$BUILD_DIR"
BLD=$?
[ $BLD -ne 0 ] && echo "!! build 失败" && exit $BLD

EXE="$BUILD_DIR/bin/BenchBoard.exe"
echo ">> 构建成功: $EXE"

# ---------- 运行（可选）----------
# 必须把 Qt 的 bin 加回 PATH，否则找不到 Qt6Core.dll 而静默退出
if [ "${1:-}" = "run" ]; then
  export PATH="$QT_DIR_MSYS/bin:$PATH"
  echo ">> 启动 BenchBoard"
  "$EXE"
fi
