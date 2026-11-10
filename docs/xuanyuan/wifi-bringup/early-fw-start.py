#!/usr/bin/env python3
"""Applied from mk-ath12k.sh: does the firmware want wlan cfg/mode before FW_READY?  Send it at
   the end of load_bdf (logged), and log each sub-step of ath12k_qmi_firmware_start so the
   -EINVAL it returns is attributed."""
import pathlib, sys
p = pathlib.Path("/home/hcy/hakios-arm64e-xuanyuan/base/xnu-deps/build/ath12k/gen/ath12k-src/qmi.c")
s = p.read_text()

# 1. sub-step logs inside ath12k_qmi_firmware_start
steps = [
    ("\tret = ath12k_qmi_wlanfw_wlan_ini_send(ab);", "wlan_ini"),
    ("\tret = ath12k_qmi_wlanfw_wlan_cfg_send(ab);", "wlan_cfg"),
    ("\tret = ath12k_qmi_wlanfw_mode_send(ab, mode);", "mode_send"),
]
if "nw: wlan_ini" not in s:
    for call, name in steps:
        assert s.count(call) == 1, "mark " + name
        s = s.replace(call, call + '\n\tath12k_dbg(ab, ATH12K_DBG_QMI, "nw: %s -> %%d\\n", ret);' % name, 1)
    print("firmware_start sub-step logs added")

# 2. the early call at the end of load_bdf
if "nw: early wlan cfg" not in s:
    anchor = '''	if (nw_qmi_qdss_cfg_send(ab))
		ath12k_warn(ab, "qmi qdss: trace configuration not sent\\n");
	ret = 0;
	return ret;
}'''
    assert s.count(anchor) == 1, "load_bdf tail"
    new = '''	if (nw_qmi_qdss_cfg_send(ab))
		ath12k_warn(ab, "qmi qdss: trace configuration not sent\\n");
	{
		int fr = ath12k_qmi_firmware_start(ab, (u32)ab->fw_mode);

		ath12k_dbg(ab, ATH12K_DBG_QMI, "nw: early wlan cfg/mode %d\\n", fr);
	}
	ret = 0;
	return ret;
}'''
    s = s.replace(anchor, new, 1)
    print("early call added")

p.write_text(s)
print("early-fw-start patch applied")
