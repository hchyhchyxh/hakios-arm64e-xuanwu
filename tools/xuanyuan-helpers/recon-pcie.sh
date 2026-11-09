#!/bin/bash
set -u
D=/mnt/x/ios/work/metal-refs/pcie
mkdir -p "$D"
F=/mnt/x/ios/work/sm8750-refs/sm8750.dtsi
echo "=== pcie nodes in sm8750.dtsi ==="
grep -nE "pcie[0-9]?: ?pci|pcie[0-9]?: pci|pcie@|pcie_phy|pcie-phy|compatible = \"qcom,pcie" "$F" | head -20
echo
N=$(grep -n "pcie0:" "$F" | head -1 | cut -d: -f1)
echo "pcie0 node at $N"
[ -n "$N" ] && sed -n "${N},$((N+55))p" "$F"
GW=$(ip route | awk '/default/ {print $3; exit}')
export http_proxy=http://$GW:12001 https_proxy=http://$GW:12001
KB=https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/plain
fetch() { curl -sL --max-time 50 -A curl "$1" -o "$D/$2" -w "%{http_code} %{size_download} $2\n" || true; }
fetch "$KB/drivers/pci/controller/dwc/pcie-qcom.c" pcie-qcom.c
fetch "$KB/drivers/pci/controller/dwc/pcie-designware-host.c" dw-host.c
ls -la "$D"
