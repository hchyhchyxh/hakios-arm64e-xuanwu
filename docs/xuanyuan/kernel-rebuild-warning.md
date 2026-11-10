# Kernel rebuild warning (2026-11-10, 20:45)

The kernel binary that runs touch on the phone is **`kernel-build/mach.xuanyuan.working-20261109`**
(SHA256 `922098fb29f677993c92ce92750d0d31e532feced7a7dac00b6d0a1d28b817eb`, 25,118,928 bytes),
kept in `X:/ios/work/kernel-backup/` and as the current
`X:/air6/xiaomi15ultra-hakios/kernel-build/mach.xuanyuan.copyio-fixed`.

**Do not rebuild the kernel from the tree as it stands.** `rebuild-verified-kernel.sh` compiles
`xnu-src/isolated/xnu-xnu-12377.121.6`, which is a copy of the tracked tree, and the tracked
`bsd/dev/hakios_dada_touch.c` is the **older SPI-bitbang driver** (`the serial engine has no SPI
firmware: driving the pins by hand` … `the chip does not identify itself` on this phone). The
kernel that works was built from a tree that also carried the **GPI** touch driver
(strings in it: `dtouch: GPI memory-only ...`, `GPI SMMU probe`, `kept lines (dtouch, redwood26,
exec)`); those changes are not in the tracked source — they exist as the patch series in
`docs/xuanyuan/patches/` (`xnu-gpi-*.patch`, `xnu-ultra-touch-*.patch`,
`xnu-ultra-tcm-response-order-and-status.patch`, `xnu-tcm-*.patch`, `xnu-touch-*.patch`) and
have to be applied to a fresh isolated tree before a build. No script does that today; the old
build did it by hand.

Consequence seen tonight: a rebuild without the patch series produced a kernel where touch never
identifies the chip, and the artifact was overwritten. It was recovered from the still-existing
`ios26-metal-scan.bin` payload (XNUP entry `kernel`, 25,118,928 bytes) with
`scripts/extract-payload-kernel.py`, and that is what the phone runs now.

Next step for a reproducible kernel build: apply the patch series to a fresh
`make-isolated-tree.sh` copy, verify `hakios_dada_touch.c` contains `GPI memory-only`, build, and
diff the result against the working binary before flashing.
