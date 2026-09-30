#!/usr/bin/env bash
# M3 验收探针一键跑法
#
#   bash tools/probes/m3_e2e_probe_run.sh normal   # 自然跑完（默认）
#   bash tools/probes/m3_e2e_probe_run.sh stop     # 第 4 秒点「停止」
#
# 它会自己做四件事：
#   1. 起一个本机 HTTP 服务当被测目标（python http.server，端口 8899）
#   2. 编译探针（链接你真实的 src/MainWindow.cpp + MetricsAggregator.cpp）
#   3. 用真实 windows 平台跑界面、自动填 URL / 设 8 VU / 点开始，每 500ms 采样状态栏
#   4. 打印断言结果，并把日志与状态栏序列存档
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

MODE="${1:-normal}"
PY="${PYTHON:-python}"
D="$ROOT/out/probes-tmp/m3e2e"
REPORTS="$ROOT/reports"

mkdir -p "$D" "$REPORTS"
cp "$ROOT/tools/probes/m3_e2e_probe.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/m3_e2e_probe.CMakeLists.txt" "$D/CMakeLists.txt"
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
for l in bad[:14]: print('   ', l.strip()[:170])
"

"$PY" -c "
import os, shutil, time
p = os.path.join(r'$REPORTS', 'summary.json')
b = os.path.join(r'$D', 'summary.json.bak')
if os.path.exists(p):
    shutil.copy2(p, b)
    print('已备份旧 summary.json（mtime %s, %d 字节）-> %s'
          % (time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(os.path.getmtime(p))),
             os.path.getsize(p), b))
    print('★ 注意：它是【上一轮】留下的 —— 本探针【不删】它，')
    print('  这样\"强杀后读到陈旧文件\"这个坑才留得下来（docs/02 3.18）。')
else:
    print('（原本没有 summary.json）')
"

"$PY" -m http.server 8899 --directory "$D" > /dev/null 2>&1 &
SRV=$!
sleep 2

echo "=== 运行 M3 端到端探针（真实 windows 平台，8 VU / 10s，mode=$MODE）==="
export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
QT_QPA_PLATFORM=windows timeout 40 ./out/m3e2e.exe "$D" "$MODE" > probe.log 2>&1
echo "probe exit=$?"

kill $SRV 2>/dev/null

echo "=== probe.log ==="
"$PY" -c "print(open(r'$D/probe.log','rb').read().decode('utf-8',errors='replace'))"

echo "=== summary.json ==="
ls -la "$REPORTS/summary.json" 2>/dev/null || echo "(没有 summary.json)"
echo "★ 判据是【mtime 是否本轮】而不是【文件在不在】——"
echo "  强杀既不写也不删，文件会停在上一轮（VERDICT=STALE，见 docs/02 3.18）。"
echo "  本探针【不删】它：留着才测得出这个坑。"

echo "=== 残留 k6 进程 ==="
tasklist //FI "IMAGENAME eq k6.exe" 2>/dev/null | grep -i k6 || echo "(无 k6 残留)"

echo "=== 产物 ==="
ls -la "$D"/m3_e2e.png "$D"/log_dump.txt "$D"/status_samples.txt 2>/dev/null
