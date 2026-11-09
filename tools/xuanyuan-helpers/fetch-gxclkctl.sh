#!/bin/bash
set -u
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
D=/mnt/x/ios/work/metal-refs/sm8750
mkdir -p "$D"
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain/drivers/clk/qcom
for f in gxclkctl-sm8750.c gxclkctl.c clk-gxclkctl.c; do
  curl -sL --max-time 40 -A curl "$KB/$f" -o "$D/$f" -w "%{http_code} %{size_download} $f\n" || true
done
echo
echo "=== remaining gcc GPU branch offsets ==="
python3 - <<'PY'
import re
src = open('/mnt/x/ios/work/metal-refs/sm8750/gcc-sm8750.c').read()
for name in ('gcc_gpu_gemnoc_gfx_clk', 'gcc_qmip_gpu_ahb_clk', 'gcc_gpu_gpll0_clk_src', 'gcc_gpu_gpll0_div_clk_src', 'gcc_ddrss_gpu_axi_clk'):
    m = re.search(r'static struct clk_branch %s = \{(.*?)\n\};' % name, src, re.S)
    if not m:
        print(name, 'NOT FOUND'); continue
    body = m.group(1)
    vals = dict(re.findall(r'\.(halt_reg|enable_reg|halt_check)\s*=\s*([^,]+),', body))
    print(name, {k: v.strip() for k, v in vals.items()})
PY
echo
echo "=== gxclkctl files ==="
ls -la "$D"/gxclkctl* 2>/dev/null
for f in "$D"/gxclkctl-sm8750.c "$D"/gxclkctl.c; do [ -s "$f" ] && { echo "--- $f ---"; grep -nE "GDSC|\.reg = 0x|domain" "$f" | head -20; }; done
