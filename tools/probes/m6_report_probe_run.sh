#!/usr/bin/env bash
# M6 报告验收 —— 跑法：bash tools/probes/m6_report_probe_run.sh
#
# ★ 不需要 QProcess / k6（本机执行上下文里 QProcess 起不了子进程，见 docs/02 §3.22）。
#
# 会【临时改写】reports/ 下的东西（Part D 要装语料 + 真点一次导出）：
#   - reports/summary.json      跑完还原
#   - reports/*.md              ★ 导出产物可能与你的同名（都是 benchboard-<时间戳>.md），
#                               所以整目录备份 → 跑完先清掉本次产物再还原
# 备份在 out/probes-tmp/m6probe/backup/
set -u
# --- 仓库根目录 ---------------------------------------------------------
# ★ 必须用 pwd -W 拿 Windows 形式（E:/...）：原生 cmake.exe 不认识 MSYS 的 /e/...
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && (pwd -W 2>/dev/null || pwd))"

export PATH="/usr/bin:/bin:/c/Windows/System32:/c/Windows"

D="$ROOT/out/probes-tmp/m6probe"
REPORTS="$ROOT/reports"
BKP="$D/backup"
SNAP="$D/snapshots"        # ★ 只增不删的历史快照

mkdir -p "$D" "$BKP" "$REPORTS"
cp "$ROOT/tools/probes/m6_report_probe.cpp"            "$D/main.cpp"
cp "$ROOT/tools/probes/m6_report_probe.CMakeLists.txt" "$D/CMakeLists.txt"

# --- 备份用户产物 ---
rm -rf "$BKP"
mkdir -p "$BKP" "$SNAP"
if [ -f "$REPORTS/summary.json" ]; then
  cp "$REPORTS/summary.json" "$BKP/summary.json"
  # ★ 另存一份只增不删的历史快照：万一某次备份到的已经是脏数据，
  #   还能从更早的快照里救回来（本轮真的发生过一次，见 restore 里的说明）。
  cp "$REPORTS/summary.json" "$SNAP/summary_$(date +%Y%m%d-%H%M%S).json"
  echo "已备份 summary.json（$(stat -c%s "$REPORTS/summary.json") 字节, md5 $(md5sum "$REPORTS/summary.json" | cut -c1-12)）"
fi
if ls "$REPORTS"/*.md >/dev/null 2>&1; then
  cp "$REPORTS"/*.md "$BKP"/
  echo "已备份 reports/*.md : $(cd "$BKP" && ls *.md | tr '\n' ' ')"
fi

restore() {
  # ★ 双保险，理由是踩过的坑：
  #   探针一旦真的拉起 k6（沙箱内 QProcess::start() 报 FailedToStart，但子进程
  #   【真的被创建了】），那个孤儿 k6 会在探针退出【之后】写 summary.json，
  #   把我们刚还原的文件冲掉 —— 实测把用户 4168 B / 1980 请求的产物冲成
  #   3290 B / 8 请求。所以：先尽力清残留 → 再还原 → 最后核验一遍。
  taskkill //F //IM k6.exe >/dev/null 2>&1 || true
  sleep 1

  local i
  for i in 1 2 3; do
    rm -f "$REPORTS"/*.md "$REPORTS/summary.json"
    if [ -f "$BKP/summary.json" ]; then cp "$BKP/summary.json" "$REPORTS/summary.json"; fi
    if ls "$BKP"/*.md >/dev/null 2>&1; then cp "$BKP"/*.md "$REPORTS"/; fi
    sleep 1
    # 核验：备份里有 summary 就必须与还原后的一致，否则再还原一次
    if [ ! -f "$BKP/summary.json" ] || cmp -s "$BKP/summary.json" "$REPORTS/summary.json"; then
      break
    fi
    echo "  ⚠️ 产物在还原后又被改写了，重试（第 $i 次）"
  done
  echo "已还原 reports/（summary.json + $(cd "$BKP" && ls *.md 2>/dev/null | wc -l) 个 .md）"
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
export QT_FORCE_STDERR_LOGGING=1

echo ""
timeout 120 ./out/m6report.exe 2>&1
echo "probe exit=$?"
