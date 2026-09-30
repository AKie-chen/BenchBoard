#!/usr/bin/env bash
# 语料回放探针一键跑法（不碰界面、不碰 k6，1 秒内出结果）
#
#   bash tools/probes/corpus_replay_probe_run.sh
#
# 它把 tests/corpus/k6_stream_sample_2k.jsonl 回放给真实的 MetricsAggregator：
#   模式① 整块喂   ← 一次 feed(2000 行)
#   模式② 每 7 字节喂一次 ← 强迫管道边界断在行中间，逼出半行残留路径
# 两种模式的结果必须完全一致，再对账固定期望值，外加 5 条边界断言。
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/corpus"
mkdir -p "$D"
cp "$ROOT/tools/probes/corpus_replay_probe.cpp"            "$D/main.cpp"
cp "$ROOT/tools/probes/corpus_replay_probe.CMakeLists.txt"  "$D/CMakeLists.txt"
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
grep -E "warning C|error C|LNK[0-9]" build.log | head -10 || echo "(build 0 警告 0 错误)"

echo "=== replay ==="
export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
./out/corpus_replay.exe "$ROOT/tests/corpus/k6_stream_sample_2k.jsonl"
echo "probe exit=$?"
