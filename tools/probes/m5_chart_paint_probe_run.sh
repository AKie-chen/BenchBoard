#!/usr/bin/env bash
# M5 图表绘制体检 —— 跑法：bash tools/probes/m5_chart_paint_probe_run.sh
#
# 链接真实的 src/RealtimeChartView.cpp，做两件事：
#   Part A 裸 QChart 的顺序 A/B（attachAxis 在前 vs addSeries 在前）
#   Part B 真实 RealtimeChartView 喂 10 个样本，逐张图报「点/轴/彩色像素」
# 产物：PNG 截图 + 控制台表格，都在 $ROOT/out/probes-tmp/m5paint/
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/m5paint"
mkdir -p "$D"
cp "$ROOT/tools/probes/m5_chart_paint_probe.cpp"            "$D/m5_chart_paint_probe.cpp"
cp "$ROOT/tools/probes/m5_chart_paint_probe.CMakeLists.txt" "$D/CMakeLists.txt"
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
timeout 120 ./out/m5paint.exe
echo "probe exit=$?"
echo ""
echo "PNG 产物：$D/paint_probe_view*.png"
