#!/usr/bin/env python3
"""Applied by mk-ath12k.sh right before compiling: add a log line before each step of
   ath12k_core_start() and the QMI firmware-ready path (gen/ath12k-src/core.c)."""
import pathlib, sys
p = pathlib.Path("/home/hcy/hakios-arm64e-xuanyuan/base/xnu-deps/build/ath12k/gen/ath12k-src/core.c")
s = p.read_text()
if "nw core:" in s:
    print("core logs already applied"); sys.exit(0)

marks = [
    "\tret = ath12k_wmi_attach(ab);",
    "\tret = ath12k_htc_init(ab);",
    "\tret = ath12k_hif_start(ab);",
    "\tret = ath12k_htc_wait_target(&ab->htc);",
    "\tret = ath12k_dp_htt_connect(ath12k_ab_to_dp(ab));",
    "\tret = ath12k_wmi_connect(ab);",
    "\tret = ath12k_htc_start(&ab->htc);",
    "\tret = ath12k_wmi_wait_for_service_ready(ab);",
    "\tret = ath12k_core_start_firmware(ab, ab->fw_mode);",
    "\tret = ath12k_ce_init_pipes(ab);",
    "\tret = ath12k_dp_cmn_device_init(ath12k_ab_to_dp(ab));",
]
count = 0
for m in marks:
    if s.count(m) != 1:
        print("mark not unique:", m, s.count(m)); sys.exit(1)
    s = s.replace(m, '\tath12k_dbg(ab, ATH12K_DBG_QMI, "nw core: %s\\n");\n%s' % (m.split("=")[1].strip().rstrip(";")[:48], m), 1)
    count += 1
p.write_text(s)
print("core logs applied:", count)

# and the QMI event types
import subprocess
subprocess.run(["python3", "/mnt/x/ios/scripts/qmi-event-log.py"], check=True)
