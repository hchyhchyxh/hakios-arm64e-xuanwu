#!/usr/bin/env python3
"""Applied by mk-ath12k.sh before compiling: log every QMI driver event type, so the run shows
   whether the firmware ever posts FW_READY after the M3 image download."""
import pathlib, sys
p = pathlib.Path("/home/hcy/hakios-arm64e-xuanyuan/base/xnu-deps/build/ath12k/gen/ath12k-src/qmi.c")
s = p.read_text()
if "nw qmi event:" in s:
    print("qmi event log already applied"); sys.exit(0)

mark = "\t\tswitch (event->type) {"
assert s.count(mark) == 1, "switch mark count " + str(s.count(mark))
s = s.replace(mark, '\t\tath12k_dbg(ab, ATH12K_DBG_QMI, "nw qmi event: %d\\n", event->type);\n' + mark, 1)
p.write_text(s)
print("qmi event log applied")
