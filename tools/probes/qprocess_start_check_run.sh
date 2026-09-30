#!/usr/bin/env bash
# QProcess 启动诊断 —— 一键跑法
#   bash tools/probes/qprocess_start_check_run.sh [k6路径]
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"
export PATH="/usr/bin:/bin:$PATH"

PROG="${1:-C:/Program Files/k6/k6.exe}"
D="$ROOT/out/probes-tmp/qpcheck"
mkdir -p "$D"
cp "$ROOT/tools/probes/qprocess_start_check.cpp"           "$D/main.cpp"
cp "$ROOT/tools/probes/qprocess_start_check.CMakeLists.txt" "$D/CMakeLists.txt"
cd "$D" || exit 1

VS_ROOT="${VS_ROOT:-/d/VS2022}"
MSVC_VER="$(ls "$VS_ROOT/VC/Tools/MSVC/" 2>/dev/null | sort -V | tail -1)"
SDK_VER="$(ls "/c/Program Files (x86)/Windows Kits/10/Include/" 2>/dev/null | grep '^10\.' | sort -V | tail -1)"
MSVC_WIN="${VS_ROOT_WIN:-D:/VS2022}/VC/Tools/MSVC/$MSVC_VER"
SDK_WIN="C:/Program Files (x86)/Windows Kits/10"
CLEANPATH="${VS_ROOT:-/d/VS2022}/VC/Tools/MSVC/$MSVC_VER/bin/HostX64/x64"
CLEANPATH="$CLEANPATH:/c/Program Files (x86)/Windows Kits/10/bin/$SDK_VER/x64"
CLEANPATH="$CLEANPATH:${QT_TOOLS_DIR:-/e/Qt/Tools}/CMake_64/bin:${QT_TOOLS_DIR:-/e/Qt/Tools}/Ninja:/c/Windows/System32:/c/Windows"
export INCLUDE="$MSVC_WIN/include;$SDK_WIN/Include/$SDK_VER/ucrt;$SDK_WIN/Include/$SDK_VER/shared;$SDK_WIN/Include/$SDK_VER/um;$SDK_WIN/Include/$SDK_VER/winrt"
export LIB="$MSVC_WIN/lib/x64;$SDK_WIN/Lib/$SDK_VER/ucrt/x64;$SDK_WIN/Lib/$SDK_VER/um/x64"

PATH="$CLEANPATH" cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DBENCHBOARD_ROOT="$ROOT" -DCMAKE_PREFIX_PATH="${QT_DIR:-E:/Qt/6.10.2/msvc2022_64}" \
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl > cfg.log 2>&1
echo "configure exit=$?"
PATH="$CLEANPATH" cmake --build out > build.log 2>&1
echo "build exit=$?"
grep -E "warning C|error C|LNK[0-9]" build.log | head -5

export PATH="/e/Qt/6.10.2/msvc2022_64/bin:$PATH"
./out/qprocess_start_check.exe "$PROG"
echo "exit=$?"
