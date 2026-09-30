#!/usr/bin/env bash
# SummaryParser 单测一键跑法
#
#   bash tools/probes/summary_parser_probe_run.sh
#
# 不需要界面、不需要 QProcess、不需要 k6 —— 纯喂 JSON，1 秒出结果。
# 所以它在这个"QProcess 起不了子进程"的环境里照样能跑（对比 m3_e2e_probe 会报 ENV_NO_QPROCESS）。
#
# 4 个用例：
#   1. 语料样本                   → 4375 / avg 3.0926 / p95 4.0206 / p99 4.8286
#   2. 真实产出形态（含 thresholds 子对象）→ 1112 / p99 12.1281
#      ★ 这个用例专门盯「语料里没有、真实产出里有」的那个键 —— 语料全绿 ≠ 真实正确
#   3. 缺 p(99)                   → 必须【解析成功】，只是该字段留 0
#   4. 坏输入                     → 必须失败，且 errorOut 里有可读原因
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32"

D="$ROOT/out/probes-tmp/spcheck"
mkdir -p "$D"
cp "$ROOT/tools/probes/summary_parser_probe.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/summary_parser_probe.CMakeLists.txt" "$D/CMakeLists.txt"
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
BAD=$(grep -acE "error C|warning C|LNK[0-9]" build.log)
echo "build issues: $BAD"
grep -aE "error C|warning C|LNK[0-9]" build.log | head -10

echo "=== run ==="
# ★ 运行时必须把 Qt 的 bin 放进 PATH，否则找不到 Qt6Core.dll（exit=127，且没有报错信息）
PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH" ./out/summary_parser_probe.exe
echo "probe exit=$?"
