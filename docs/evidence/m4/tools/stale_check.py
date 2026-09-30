"""A/B 实测：--summary-export 的文件在 k6 被强杀后会不会被刷新。

A：正常跑完 -> 文件写入本轮数据
B：同一个输出路径，跑 30s 但 3s 时强杀 -> 看文件内容/mtime
结论用于 M5：不能只看 "summary.json 存在与否"，必须看引擎是否正常退出。
"""
import subprocess, os, time, json

K6     = r"C:\Program Files\k6\k6.exe"
SCRIPT = r"E:\BenchBoard\examples\script_demo.js"
OUT    = r"E:\BenchBoard\.workbuddy\tmp\stale_summary.json"
URL    = "http://127.0.0.1:8899/"

if os.path.exists(OUT):
    os.remove(OUT)

def run(program, args):
    return subprocess.Popen([program] + args,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)

# ---------- A: 正常跑完 ----------
a = run(K6, ["run", "--quiet", "--summary-export", OUT,
             "--vus", "2", "--duration", "3s", "-e", "BASE_URL=" + URL, SCRIPT])
ao, ae = a.communicate(timeout=90)
print("A  rc =", a.returncode)
print("A  file exists =", os.path.exists(OUT))
mA = os.path.getmtime(OUT)
dA = json.load(open(OUT, encoding="utf-8"))
print("A  http_reqs.count =", dA["metrics"]["http_reqs"]["count"])
print("A  mtime =", time.strftime("%H:%M:%S", time.localtime(mA)))

print()

# ---------- B: 同一个路径，强杀 ----------
# 先睡 2 秒，保证 mtime 若被刷新一定能看出来
time.sleep(2)
b = run(K6, ["run", "--quiet", "--summary-export", OUT,
             "--vus", "2", "--duration", "30s", "-e", "BASE_URL=" + URL, SCRIPT])
time.sleep(3)                      # 让它真跑起来、发出若干请求
t_kill = time.time()
b.kill()
b.wait()
print("B  killed at", time.strftime("%H:%M:%S", time.localtime(t_kill)))
print("B  file exists =", os.path.exists(OUT))
if os.path.exists(OUT):
    mB = os.path.getmtime(OUT)
    dB = json.load(open(OUT, encoding="utf-8"))
    print("B  http_reqs.count =", dB["metrics"]["http_reqs"]["count"])
    print("B  mtime =", time.strftime("%H:%M:%S", time.localtime(mB)))
    print()
    print("VERDICT =", "STALE（文件还是上一轮的）" if abs(mB - mA) < 0.5
                        else "UPDATED（被刷新了）")
else:
    print("VERDICT = FILE_GONE（文件被删/没产出）")
