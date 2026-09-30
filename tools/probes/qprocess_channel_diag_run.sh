#!/usr/bin/env bash
# QProcess 通道模式诊断 —— 跑法：bash tools/probes/qprocess_channel_diag_run.sh
#
# 依次尝试三种通道模式（Separate / Forwarded / Merged+文件），打印每种模式下
#   ① QProcess::errorString()          ← ★ 注意：它可能是【陈旧】的 LastError
#   ② Qt 自己 qWarning 的真实失败点    ← ★ 这才是真错误码
#
# 2026-09-25 实测结论：三种模式全部失败，真实原因是
#   QProcess: CreateFile failed. (所有的管道范例都在使用中。)  = ERROR_PIPE_BUSY
# 详见 docs/02 §3.22。
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"
export PATH="/usr/bin:/bin:$PATH"

D="$ROOT/out/probes-tmp/qpdiag"
mkdir -p "$D"
cp "$ROOT/tools/probes/qprocess_channel_diag.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/qprocess_channel_diag.CMakeLists.txt" "$D/CMakeLists.txt"
cd "$D" || exit 1

# --- 自动探测工具链版本（不要硬编码，升级 VS/SDK 后脚本会静默失败）---
MSVC_VER="$(ls ${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/ 2>/dev/null | sort -V | tail -1)"
SDK_VER="$(ls "/c/Program Files (x86)/Windows Kits/10/Include/" 2>/dev/null | grep '^10\.' | sort -V | tail -1)"
if [ -z "$MSVC_VER" ] || [ -z "$SDK_VER" ]; then
  echo "!! 找不到 MSVC 或 Windows SDK，检查 ${VS_ROOT_WIN:-D:/VS2022} 与 C:/Program Files (x86)/Windows Kits/10"
  exit 1
fi
echo "MSVC=$MSVC_VER  SDK=$SDK_VER"

MSVC_WIN="${VS_ROOT_WIN:-D:/VS2022}/VC/Tools/MSVC/$MSVC_VER"
SDK_WIN="C:/Program Files (x86)/Windows Kits/10"
CLEANPATH="${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/$MSVC_VER/bin/HostX64/x64"
CLEANPATH="$CLEANPATH:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64"
CLEANPATH="$CLEANPATH:${QT_TOOLS_DIR:-/e/Qt/Tools}/CMake_64/bin:${QT_TOOLS_DIR:-/e/Qt/Tools}/Ninja:/c/Windows/System32:/c/Windows"
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
grep -E "warning C|error C|LNK[0-9]" build.log | head -8 || echo "(build 0 警告 0 错误)"

export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"

echo ""
echo "=== 运行（★ QT_LOGGING_TO_CONSOLE=1：不加的话 qWarning 走 OutputDebugString，一条都看不见）==="
QT_LOGGING_TO_CONSOLE=1 ./out/qprocess_channel_diag.exe 2>&1
echo "exit=$?"
