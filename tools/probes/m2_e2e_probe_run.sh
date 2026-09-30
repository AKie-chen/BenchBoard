#!/usr/bin/env bash
export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

PY="${PYTHON:-python}"
D="$ROOT/out/probes-tmp/m2e2e"
REPORTS="$ROOT/reports"
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
"$PY" -c "
import re
t=open(r'$D/build.log','rb').read().decode('utf-8',errors='replace')
bad=[l for l in t.splitlines() if re.search(r'error C|warning C|LNK[0-9]',l)]
print('build issues:', len(bad))
for l in bad[:12]: print('   ', l.strip()[:170])
"

"$PY" -c "
import os
p=r'$REPORTS/summary.json'
if os.path.exists(p): os.remove(p)
print('cleared old summary.json')
"
mkdir -p "$REPORTS"

"$PY" -m http.server 8899 --directory "$D" > /dev/null 2>&1 &
SRV=$!
sleep 2

echo "=== 运行端到端探针（真实 windows 平台，自动点「开始压测」，5 VU / 10s）==="
export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
QT_QPA_PLATFORM=windows timeout 40 ./out/m2e2e.exe "$D" > probe.log 2>&1
echo "probe exit=$?"

kill $SRV 2>/dev/null

echo "=== probe.log ==="
"$PY" -c "print(open(r'$D/probe.log','rb').read().decode('utf-8',errors='replace'))"

echo "=== summary.json 是否产出 ==="
ls -la "$REPORTS/summary.json" 2>/dev/null || echo "(无 summary.json)"

echo "=== 残留 k6 进程 ==="
tasklist //FI "IMAGENAME eq k6.exe" 2>/dev/null | grep -i k6 || echo "(无 k6 残留)"
