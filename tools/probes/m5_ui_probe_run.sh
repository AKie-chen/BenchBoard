#!/usr/bin/env bash
# M5 界面级验收 —— 跑法：bash tools/probes/m5_ui_probe_run.sh
#
# ★ 本探针【不需要 QProcess / k6】—— 本机执行上下文里 QProcess 起不了子进程
#   （QProcess: CreateFile failed = ERROR_PIPE_BUSY，见 docs/02 §3.22），
#   所以它用 QMetaObject::invokeMethod 直接驱动 MainWindow 的私有槽。
#
# 会【临时改写】reports/summary.json（Part D 要造一份"上一轮"的陈旧文件），
# 跑完自动还原。备份在 out/probes-tmp/m5ui/summary.json.bak
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/m5ui"
REPORTS="$ROOT/reports"
BKP="$D/summary.json.bak"

mkdir -p "$D" "$REPORTS"
cp "$ROOT/tools/probes/m5_ui_probe.cpp"            "$D/main.cpp"
cp "$ROOT/tools/probes/m5_ui_probe.CMakeLists.txt" "$D/CMakeLists.txt"

# --- 备份用户的 summary.json ---
if [ -f "$REPORTS/summary.json" ]; then
  cp "$REPORTS/summary.json" "$BKP"
  echo "已备份 summary.json -> $BKP"
else
  echo "（原本没有 summary.json）"
fi

restore() {
  if [ -f "$BKP" ]; then
    cp "$BKP" "$REPORTS/summary.json"
    echo "已还原 summary.json"
  fi
}
trap restore EXIT

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
timeout 120 ./out/m5ui.exe
echo "probe exit=$?"
echo ""
echo "PNG 产物：$D/ui_probe_view*.png"
