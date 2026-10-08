"""Summarises UE ProfileGPU table output from a log: inclusive time per pass, limited tree depth.
Usage: python3 -I Scripts/parse_profilegpu.py <log> [max_depth=4] [min_ms=0.1]
"""
import re
import sys

log_path = sys.argv[1]
max_depth = int(sys.argv[2]) if len(sys.argv) > 2 else 4
min_ms = float(sys.argv[3]) if len(sys.argv) > 3 else 0.1

lines = open(log_path, encoding="utf-8", errors="replace").read().splitlines()
start = next((i for i, l in enumerate(lines) if "GPU Profile for Frame" in l), None)
if start is None:
    sys.exit("no ProfileGPU output found")

for line in lines[start:start + 6]:
    if "Frame Time" in line:
        print(line.split("LogRHI: Display:")[-1].strip())

row = re.compile(r"┃(?P<excl>[^┃]*)┃(?P<incl>[^┃]*)┃(?P<name>[^┃]*)┃")
for line in lines[start:]:
    if "LogRHI" not in line:
        break
    m = row.search(line)
    if not m:
        continue
    incl_cols = [c.strip() for c in m.group("incl").split("│")]
    time_text = incl_cols[-1]
    if not time_text.endswith("ms"):
        continue
    ms = float(time_text.split()[0])
    name_field = m.group("name").rstrip()
    stripped = name_field.lstrip()
    depth = (len(name_field) - len(stripped)) // 2
    if depth <= max_depth and ms >= min_ms:
        print(f"{ms:7.2f} ms  {'  ' * depth}{stripped}")
