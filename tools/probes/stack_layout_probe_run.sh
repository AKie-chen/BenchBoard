#!/usr/bin/env bash
# stack_layout_probe 跑法：bash tools/probes/stack_layout_probe_run.sh
#
# 回答一个问题：QVBoxLayout 写成栈对象，是"崩"，还是只是"失去布局"？
# 三种形态各跑一遍，拿退出码 + 控件几何尺寸说话。
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/stacklayout"
mkdir -p "$D"
cp "$ROOT/tools/probes/stack_layout_probe.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/stack_layout_probe.CMakeLists.txt" "$D/CMakeLists.txt"
cd "$D" || exit 1

export MSVC_WIN="${VS_ROOT_WIN:-D:/VS2022}/VC/Tools/MSVC/14.44.35207"
export SDK_VER="10.0.26100.0"
export SDK_WIN="C:/Program Files (x86)/Windows Kits/10"
CLEANPATH="${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/14.44.35207/bin/HostX64/x64:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64:${QT_TOOLS_DIR:-/e/Qt/Tools}/CMake_64/bin:${QT_TOOLS_DIR:-/e/Qt/Tools}/Ninja:/c/Windows/System32:/c/Windows"
export INCLUDE="$MSVC_WIN/include;$SDK_WIN/Include/$SDK_VER/ucrt;$SDK_WIN/Include/$SDK_VER/shared;$SDK_WIN/Include/$SDK_VER/um;$SDK_WIN/Include/$SDK_VER/winrt"
export LIB="$MSVC_WIN/lib/x64;$SDK_WIN/Lib/$SDK_VER/ucrt/x64;$SDK_WIN/Lib/$SDK_VER/um/x64"

echo "=== configure ==="
PATH="$CLEANPATH" cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBENCHBOARD_ROOT="$ROOT" -DCMAKE_PREFIX_PATH="${QT_DIR:-E:/Qt/6.10.2/msvc2022_64}" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl > cfg.log 2>&1
echo "configure exit=$?"

echo "=== build ==="
PATH="$CLEANPATH" cmake --build out > build.log 2>&1
echo "build exit=$?"
grep -E "warning C|error C|LNK[0-9]" build.log | head -12 || echo "(build 0 警告 0 错误)"

export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
export QT_QPA_PLATFORM=offscreen

for m in heap stack-owner-alive stack-owner-dead stack-show-after stretch-zero; do
  echo ""
  echo "########## $m ##########"
  timeout 30 ./out/stacklayout.exe "$m"
  echo "exit=$?"
done

echo ""
echo "判读："
echo "  heap              控件的 geometry 应被撑开（布局在管）"
echo "  stack-owner-alive 看 central->layout() 是否变悬垂、控件是否还是初始尺寸、delete 崩不崩"
echo "  stack-owner-dead  这条才是 double free 形态；exit 非 0 即复现"
echo "  stack-show-after  layout 先死再 show → 控件有没有被摆过位（对得上 buildUi 的真实时序）"
echo "  stretch-zero      不传拉伸因子时，多出来的高度到底归谁"
