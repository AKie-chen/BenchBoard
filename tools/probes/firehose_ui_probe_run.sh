#!/usr/bin/env bash
# 火管 UI 探针一键跑法
#
#   bash tools/probes/firehose_ui_probe_run.sh [目标URL] [VU数] [时长秒] [构建类型] [QPA平台]
#
# 默认：http://127.0.0.1:8917/  8 VU  10s  Debug  windows
#
# ★ 构建类型默认 Debug，是为了和 apps 侧（tools/build.sh）保持一致。
#   探针若用 Release，就会重演 M3 那次「探针全绿、实机卡死」的假阳性。
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:$PATH"

URL="${1:-http://127.0.0.1:8917/}"
VUS="${2:-8}"
DUR="${3:-10}"
BUILD_TYPE="${4:-Debug}"
QPA="${5:-windows}"

D="$ROOT/out/probes-tmp/firehose_ui_$BUILD_TYPE"
REPORTS="$ROOT/reports"
NODE="${NODE:-node}"

mkdir -p "$D" "$REPORTS"
cp "$ROOT/tools/probes/firehose_ui_probe.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/firehose_ui_probe.CMakeLists.txt" "$D/CMakeLists.txt"
cd "$D" || exit 1

# ---- 工具链自动探测 ----
VS_ROOT="${VS_ROOT:-/d/VS2022}"
MSVC_VER="$(ls "$VS_ROOT/VC/Tools/MSVC/" 2>/dev/null | sort -V | tail -1)"
SDK_VER="$(ls "/c/Program Files (x86)/Windows Kits/10/Include/" 2>/dev/null | grep '^10\.' | sort -V | tail -1)"
if [ -z "$MSVC_VER" ] || [ -z "$SDK_VER" ]; then
  echo "!! 无法探测 MSVC / Windows SDK" >&2
  exit 1
fi
echo ">> 工具链: MSVC $MSVC_VER / SDK $SDK_VER / 构建类型 $BUILD_TYPE"

MSVC_WIN="${VS_ROOT_WIN:-D:/VS2022}/VC/Tools/MSVC/$MSVC_VER"
SDK_WIN="C:/Program Files (x86)/Windows Kits/10"
CLEANPATH="${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/$MSVC_VER/bin/HostX64/x64"
CLEANPATH="$CLEANPATH:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64"
CLEANPATH="$CLEANPATH:${QT_TOOLS_DIR:-/e/Qt/Tools}/CMake_64/bin:${QT_TOOLS_DIR:-/e/Qt/Tools}/Ninja:/c/Windows/System32:/c/Windows"
export INCLUDE="$MSVC_WIN/include;$SDK_WIN/Include/$SDK_VER/ucrt;$SDK_WIN/Include/$SDK_VER/shared;$SDK_WIN/Include/$SDK_VER/um;$SDK_WIN/Include/$SDK_VER/winrt"
export LIB="$MSVC_WIN/lib/x64;$SDK_WIN/Lib/$SDK_VER/ucrt/x64;$SDK_WIN/Lib/$SDK_VER/um/x64"

echo "=== configure ==="
PATH="$CLEANPATH" cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -DBENCHBOARD_ROOT="$ROOT" -DCMAKE_PREFIX_PATH="${QT_DIR:-E:/Qt/6.10.2/msvc2022_64}" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl > cfg.log 2>&1
echo "configure exit=$?"

echo "=== build ==="
PATH="$CLEANPATH" cmake --build out > build.log 2>&1
echo "build exit=$?"
grep -E "warning C|error C|LNK[0-9]" build.log | head -10 || echo "(0 警告 0 错误)"

# ---- 起一个「快目标」—— 目的是把 k6 的 JSON 输出推到万级 rps ----
# 用独立端口，避免和用户可能已经在跑的 8901/8899 撞车。
PORT="$(echo "$URL" | sed -n 's#.*:\([0-9]\+\).*#\1#p')"
if [ -z "$PORT" ]; then PORT=8917; fi
"$NODE" "$ROOT/tools/probes/fast_http_server.js" "$PORT" > server.log 2>&1 &
SRV=$!
sleep 2
if grep -q EADDRINUSE server.log 2>/dev/null; then
  echo ">> 端口 $PORT 上已有服务在监听 —— 沿用它，不另起（要跑自己的靶子就直接用这个端口）"
  SRV=""
else
  echo ">> 快目标已起：http://127.0.0.1:$PORT/（pid=$SRV）"
fi

rm -f "$REPORTS/summary.json"

echo "=== 运行火管 UI 探针（$QPA 平台，$VUS VU / ${DUR}s，$BUILD_TYPE）==="
export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY
QT_QPA_PLATFORM="$QPA" timeout 300 ./out/firehose_ui.exe "$D" "$URL" "$VUS" "$DUR" 220 > probe.log 2>&1
echo "probe exit=$?"

if [ -n "$SRV" ]; then kill $SRV 2>/dev/null; fi

echo "=== probe.log ==="
cat probe.log

echo "=== summary.json（k6 自己认为跑了多久）==="
if [ -f "$REPORTS/summary.json" ]; then
  ls -la "$REPORTS/summary.json"
  "${PYTHON:-python}" -c "
import json
d=json.load(open(r'$REPORTS/summary.json',encoding='utf-8'))
m=d.get('metrics',{})
print('  http_reqs.count =', m.get('http_reqs',{}).get('count'))
print('  http_reqs.rate  =', round(m.get('http_reqs',{}).get('rate',0),1))
print('  -> k6 实测时长  = %.2f s' % (m.get('http_reqs',{}).get('count',0)/max(1e-9,m.get('http_reqs',{}).get('rate',1))))
"
else
  echo "(无 summary.json)"
fi

echo "=== 残留 k6 进程 ==="
tasklist //FI "IMAGENAME eq k6.exe" 2>/dev/null | grep -i k6 || echo "(无 k6 残留)"

echo "=== 产物 ==="
ls -la "$D"/firehose_log.txt "$D"/firehose_ui.png 2>/dev/null
