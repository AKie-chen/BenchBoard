#!/usr/bin/env bash
# M4 骨架下发前自测 —— 跑法：bash tools/probes/m4_skeleton_check_run.sh
#
# 做两件事：
#   ① heap  模式：把 TODO 的示例代码原样编译运行，6 条断言全过 → 证明发出的 TODO 是能落地的
#   ② stack 模式：故意用栈对象写 chart/series/axis，看会不会崩 → 验证"必须堆分配"这条
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/m4check"
mkdir -p "$D"
cp "$ROOT/tools/probes/m4_skeleton_check.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/m4_skeleton_check.CMakeLists.txt" "$D/CMakeLists.txt"
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

echo ""
echo "########## ① heap 模式：TODO 示例代码能否落地 ##########"
timeout 30 ./out/m4skeleton.exe heap
echo "heap exit=$?"

echo ""
echo "########## ② stack 模式：栈分配会不会崩 ##########"
timeout 30 ./out/m4skeleton.exe stack
echo "stack exit=$?"
echo ""
echo "（stack 模式 exit 非 0 = 复现了「必须堆分配」；exit=0 = 本次未复现，如实记录）"
