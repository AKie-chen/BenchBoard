#!/usr/bin/env bash
# 火管回放基准 —— 一键跑法
#
#   bash tools/probes/firehose_replay_bench_run.sh <捕获文件> [块大小]
#
# 捕获文件怎么来（必须用【文件重定向】而不是管道，否则测的是管道不是 k6）：
#   "/c/Program Files/k6/k6.exe" run --quiet -o json=- \
#     --vus 8 --duration 10s -e BASE_URL=http://127.0.0.1:8917/ \
#     examples/script_demo.js > out/probes-tmp/capture.jsonl
#
# 注意：跑 k6 前要 unset http_proxy/https_proxy，否则可能被代理劫持。
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:$PATH"

CAPTURE="${1:-}"
CHUNK="${2:-32768}"
# ★ 第 3 个参数：构建类型。默认 Release。
#   必须能切到 Debug —— 因为 apps 侧走的是 tools/build.sh 的 Debug，
#   两者的解析速度差好几倍，不分开测就会得出错误结论（实测踩过）。
BUILD_TYPE="${3:-Release}"
if [ -z "$CAPTURE" ]; then
  echo "用法: $0 <捕获文件> [块大小字节] [Release|Debug]" >&2
  exit 2
fi

D="$ROOT/out/probes-tmp/firehose_bench_$BUILD_TYPE"
mkdir -p "$D"
cp "$ROOT/tools/probes/firehose_replay_bench.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/firehose_replay_bench.CMakeLists.txt" "$D/CMakeLists.txt"
cd "$D" || exit 1

# ---- 工具链自动探测（别硬编码版本号，VS 升级后脚本会失效）----
VS_ROOT="${VS_ROOT:-/d/VS2022}"
SDK_VER="$(ls "/c/Program Files (x86)/Windows Kits/10/Include/" 2>/dev/null | grep '^10\.' | sort -V | tail -1)"
MSVC_VER="$(ls "$VS_ROOT/VC/Tools/MSVC/" 2>/dev/null | sort -V | tail -1)"
if [ -z "$MSVC_VER" ] || [ -z "$SDK_VER" ]; then
  echo "!! 无法探测 MSVC / Windows SDK" >&2
  exit 1
fi
echo ">> 工具链: MSVC $MSVC_VER / SDK $SDK_VER"

MSVC_WIN="${VS_ROOT_WIN:-D:/VS2022}/VC/Tools/MSVC/$MSVC_VER"
SDK_WIN="C:/Program Files (x86)/Windows Kits/10"
CLEANPATH="${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/$MSVC_VER/bin/HostX64/x64"
CLEANPATH="$CLEANPATH:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64"
CLEANPATH="$CLEANPATH:${QT_TOOLS_DIR:-/e/Qt/Tools}/CMake_64/bin:${QT_TOOLS_DIR:-/e/Qt/Tools}/Ninja:/c/Windows/System32:/c/Windows"
export INCLUDE="$MSVC_WIN/include;$SDK_WIN/Include/$SDK_VER/ucrt;$SDK_WIN/Include/$SDK_VER/shared;$SDK_WIN/Include/$SDK_VER/um;$SDK_WIN/Include/$SDK_VER/winrt"
export LIB="$MSVC_WIN/lib/x64;$SDK_WIN/Lib/$SDK_VER/ucrt/x64;$SDK_WIN/Lib/$SDK_VER/um/x64"

echo "=== configure ($BUILD_TYPE) ==="
PATH="$CLEANPATH" cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DBENCHBOARD_ROOT="$ROOT" -DCMAKE_PREFIX_PATH="${QT_DIR:-E:/Qt/6.10.2/msvc2022_64}" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl > cfg.log 2>&1
echo "configure exit=$?"

echo "=== build ==="
PATH="$CLEANPATH" cmake --build out > build.log 2>&1
echo "build exit=$?"
grep -E "warning C|error C|LNK[0-9]" build.log | head -10 || echo "(0 警告 0 错误)"

echo "=== bench ==="
export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
./out/firehose_replay_bench.exe "$CAPTURE" "$CHUNK"
exit $?
