"""Runs UmapDump as one child process, polls its working set and terminates ONLY that child above a memory cap.
Writes the explicit exit code, peak working set and wall time to <output>/run-result.json."""
import json
import subprocess
import sys
import time

import psutil

cap_mb = int(sys.argv[1])
output_dir = sys.argv[2]
command = sys.argv[3:]
started = time.time()
child = subprocess.Popen(command, stdout=open(output_dir + "/run.log", "w"), stderr=subprocess.STDOUT)
process = psutil.Process(child.pid)
peak_mb = 0
killed = False
while child.poll() is None:
    try:
        peak_mb = max(peak_mb, process.memory_info().rss // (1024 * 1024))
    except psutil.Error:
        break
    if peak_mb > cap_mb:
        child.kill()
        killed = True
        break
    time.sleep(0.25)
child.wait()
json.dump({"exitCode": child.returncode, "killedForMemoryCap": killed, "capMb": cap_mb, "peakWorkingSetMb": peak_mb,
           "seconds": round(time.time() - started, 1)}, open(output_dir + "/run-result.json", "w"), indent=1)
print(open(output_dir + "/run-result.json").read())
